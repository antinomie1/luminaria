// luminaria/backend/auto_backend.cppm — pick the backend this machine can run.
//
// Every compositor built on this library starts with the same fifty lines: run
// nested when there is a parent compositor, on bare metal when there is a seat,
// headless otherwise; open the card, join the seat, attach libinput to the same
// session, and point the renderer at the GPU behind the display. `AutoBackend`
// is those fifty lines.
//
// Auto tries, in order:
//   1. nested — `WaylandBackend::create`, plus one window;
//   2. DRM — but only when a libseat `Session` can be had, so a compositor
//      started over ssh or in CI never lands on a VT by accident;
//   3. headless — one virtual output.
// Asked for one kind by name, a failure is an error rather than a fallback, and
// DRM then also runs without a session (VT switching is unsafe, see
// session.cppm; `session_error()` says why there is none).
//
// What stays the caller's: reading the environment (`parse_backend_kind` turns
// the usual auto|nested|drm|headless string into a kind), and everything after
// the pick — the outputs arrive through `backend().new_output` at `start()`,
// input through `input()`.

module;

export module luminaria.shell:auto_backend;

import std;

import luminaria;
import luminaria.backend;
import luminaria.render;

export namespace luminaria {

enum class BackendKind {
    automatic, ///< nested, then DRM with a session, then headless
    nested,
    drm,
    headless,
};

/// "auto" | "nested" | "drm" | "headless" — what `LUMINARIA_BACKEND` holds in
/// the examples. Anything else fails, naming the four.
[[nodiscard]] Result<BackendKind> parse_backend_kind(std::string_view text);

/// The name `parse_backend_kind` reads back: "auto", "nested", "drm", "headless".
[[nodiscard]] std::string_view backend_kind_name(BackendKind kind) noexcept;

struct BackendOptions {
    BackendKind kind = BackendKind::automatic;
    /// The DRM card, e.g. "/dev/dri/card1". Empty: the first one with a monitor.
    std::string drm_device;
    /// The one output nested and headless come with. DRM's are its monitors.
    int width = 1280;
    int height = 800;
    /// The nested window's title.
    std::string title = "luminaria";
};

class AutoBackend {
public:
    /// Pick and create the backend. Nothing is started: subscribe to
    /// `backend().new_output` and `input()->input()` first, then `start()`.
    [[nodiscard]] static Result<AutoBackend> create(EventLoop loop,
                                                    const BackendOptions& options = {});

    ~AutoBackend();
    AutoBackend(AutoBackend&&) noexcept;
    AutoBackend& operator=(AutoBackend&&) noexcept;
    AutoBackend(const AutoBackend&) = delete;
    AutoBackend& operator=(const AutoBackend&) = delete;

    /// What was picked; never `automatic`.
    [[nodiscard]] BackendKind kind() const noexcept;

    [[nodiscard]] Backend& backend() noexcept;
    /// Where input comes from: the nested backend, or libinput on DRM. Null
    /// headless.
    [[nodiscard]] InputSource* input() noexcept;
    /// Non-null when nested: its `close_requested` is the caller's to answer.
    [[nodiscard]] WaylandBackend* nested() noexcept;
    /// Non-null on DRM.
    [[nodiscard]] DrmBackend* drm() noexcept;
    [[nodiscard]] LibinputBackend* libinput() noexcept;
    /// Non-null when a libseat session was joined: VT switching
    /// (`switch_to`) and `activity()`. Only ever on DRM — auto lets go of a
    /// session whose DRM backend failed.
    [[nodiscard]] Session* session() noexcept;
    /// On DRM without a session (asked for by name, no seat to join): why.
    /// Empty otherwise.
    [[nodiscard]] std::string_view session_error() const noexcept;

    /// For the renderer: on DRM, `drm_device` is the card, so the GPU that
    /// renders is the one behind the display.
    [[nodiscard]] const VulkanOptions& vulkan_options() const noexcept;

    /// libinput first, then the backend: by the time `new_output` fires the
    /// input devices are open and `input()->capabilities()` already answers.
    Status start();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit AutoBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct AutoBackend::Impl {
    // Declaration order is destruction order, reversed: libinput and the card
    // let go of their devices before the session they opened them through.
    std::optional<Session> session;
    std::unique_ptr<Backend> backend;
    std::optional<LibinputBackend> libinput;
    WaylandBackend* nested = nullptr;
    DrmBackend* drm = nullptr;
    BackendKind kind = BackendKind::headless;
    std::string session_error;
    VulkanOptions vulkan;
};

Result<BackendKind> parse_backend_kind(std::string_view text) {
    for (const BackendKind k :
         {BackendKind::automatic, BackendKind::nested, BackendKind::drm, BackendKind::headless}) {
        if (text == backend_kind_name(k)) {
            return k;
        }
    }
    return fail("backend \"" + std::string(text) + "\": expected auto, nested, drm or headless");
}

std::string_view backend_kind_name(BackendKind kind) noexcept {
    switch (kind) {
    case BackendKind::automatic: return "auto";
    case BackendKind::nested: return "nested";
    case BackendKind::drm: return "drm";
    case BackendKind::headless: return "headless";
    }
    return "auto";
}

AutoBackend::AutoBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AutoBackend::~AutoBackend() = default;
AutoBackend::AutoBackend(AutoBackend&&) noexcept = default;
AutoBackend& AutoBackend::operator=(AutoBackend&&) noexcept = default;

Result<AutoBackend> AutoBackend::create(EventLoop loop, const BackendOptions& options) {
    const BackendKind want = options.kind;
    const bool any = want == BackendKind::automatic;
    auto impl = std::make_unique<Impl>();

    if (any || want == BackendKind::nested) {
        if (auto wb = WaylandBackend::create(loop); wb) {
            auto owned = std::make_unique<WaylandBackend>(std::move(*wb));
            owned->add_output(options.width, options.height, options.title);
            impl->nested = owned.get();
            impl->backend = std::move(owned);
            impl->kind = BackendKind::nested;
            return AutoBackend{std::move(impl)};
        } else if (!any) {
            return std::unexpected(wb.error());
        }
    }

    if (any || want == BackendKind::drm) {
        // With a seat a VT switch is safe: DRM master is handed back and input
        // suspended. Auto only tries DRM with one; asked for, it runs without.
        // `impl` is on the heap, so the session's address survives the moves.
        if (auto s = Session::create(loop); s) {
            impl->session.emplace(std::move(*s));
        } else {
            impl->session_error = s.error().message;
        }
        Session* session = impl->session.has_value() ? &*impl->session : nullptr;
        if (!any || session != nullptr) {
            auto drm = options.drm_device.empty()
                           ? DrmBackend::create(loop, session)
                           : DrmBackend::create(loop, options.drm_device, session);
            if (drm) {
                auto input = LibinputBackend::create(loop, session);
                if (!input) {
                    return std::unexpected(input.error());
                }
                auto owned = std::make_unique<DrmBackend>(std::move(*drm));
                impl->vulkan.drm_device = drm_device_id(owned->fd());
                impl->drm = owned.get();
                impl->backend = std::move(owned);
                impl->libinput.emplace(std::move(*input));
                impl->kind = BackendKind::drm;
                return AutoBackend{std::move(impl)};
            }
            if (!any) {
                return std::unexpected(drm.error());
            }
        }
        impl->session.reset();
        impl->session_error.clear();
    }

    auto owned = std::make_unique<HeadlessBackend>(loop);
    owned->add_output(options.width, options.height);
    impl->backend = std::move(owned);
    impl->kind = BackendKind::headless;
    return AutoBackend{std::move(impl)};
}

BackendKind AutoBackend::kind() const noexcept { return impl_->kind; }

Backend& AutoBackend::backend() noexcept { return *impl_->backend; }

InputSource* AutoBackend::input() noexcept {
    if (impl_->nested != nullptr) {
        return impl_->nested;
    }
    return impl_->libinput.has_value() ? &*impl_->libinput : nullptr;
}

WaylandBackend* AutoBackend::nested() noexcept { return impl_->nested; }

DrmBackend* AutoBackend::drm() noexcept { return impl_->drm; }

LibinputBackend* AutoBackend::libinput() noexcept {
    return impl_->libinput.has_value() ? &*impl_->libinput : nullptr;
}

Session* AutoBackend::session() noexcept {
    return impl_->session.has_value() ? &*impl_->session : nullptr;
}

std::string_view AutoBackend::session_error() const noexcept { return impl_->session_error; }

const VulkanOptions& AutoBackend::vulkan_options() const noexcept { return impl_->vulkan; }

Status AutoBackend::start() {
    if (impl_->libinput.has_value()) {
        if (auto s = impl_->libinput->start(); !s) {
            return s;
        }
    }
    return impl_->backend->start();
}

} // namespace luminaria
