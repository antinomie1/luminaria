// luminaria/shell/screen_manager.cppm — an output, from arrival to removal.
//
// Every compositor used to write this the same way: listen to `new_output`,
// `SceneRenderer::reset()`, join the layout, open a `wl_output`, connect `frame`
// to `present()`, and take all of it apart again on `destroy`. None of that is
// policy. What is policy is what the screen shows, and that is the one thing
// left to the caller: `set_scene_builder()`.
//
// Still immediate-mode (ADR 0001). This holds per-output machinery, never a
// window: the builder hands over a list when asked, and nothing of it survives
// the call. So the same list answers everything else that needs to know what is
// on screen, built when it is asked for instead of remembered from the last
// frame: `scene_at()` for input, and `follow_popups()` for a menu that has to
// stay on its monitor. A click therefore cannot disagree with the pixels.
//
// Everything `SceneRenderer` already does per output stays there: an `unchanged`
// frame releases its callbacks, a `present` releases the rest and sends
// presentation feedback, a mode change reallocates. A steady frame costs one
// call of the builder and `present()`, and allocates nothing either of them
// does not (`tests/test_screen_manager.cpp` counts it).
export module luminaria.shell:screen_manager;

import std;

import luminaria;
import luminaria.render;

import :popup;
import :scene;
import :scene_renderer;

export namespace luminaria {

/// What a `ScreenManager` does with an output on arrival.
struct ScreenOptions {
    /// What shows where the scene does not reach.
    Color background{0.0f, 0.0f, 0.0f, 1.0f};
    /// `OutputLayout::add_auto()`: right of everything already placed. Off for a
    /// compositor that arranges outputs itself, from `backend.new_output`.
    bool join_layout = true;
    /// Open a `wl_output` that follows the output and the layout.
    bool announce = true;
    /// Variable refresh on a monitor that has it: the panel follows the frames,
    /// so an idle desktop idles it and a game runs as fast as it renders.
    bool variable_refresh = false;
    /// The `wl_output`s are called "<name>-1", "<name>-2", ...; a number is
    /// never used twice, so a replugged monitor cannot collide with another.
    std::string name = "luminaria";
    /// XRGB8888 unless told otherwise.
    std::uint32_t drm_format = 0x34325258;
};

/// Something went wrong on one output. Nothing here is fatal: a render target
/// that would not allocate costs the GPU on that output, and a frame that failed
/// costs that frame. Printed to stderr unless someone is listening to `failed()`:
/// a silent fallback to the CPU is a mystery a year later.
struct ScreenFailure {
    Output& output;
    std::string_view message;
};

/// The scene changed in a way no client reports. Every output has been asked for
/// a frame by the time this fires; what is left is whatever else follows the
/// scene, like the pointer (`DefaultInputRouter` listens).
struct ScreenInvalidated {};

/// Every output of a backend, for as long as it exists: render targets, its place
/// in the layout, its `wl_output`, and a frame drawn whenever it asks for one.
///
/// Construct it before `start()`, which is when the outputs arrive. It needs the
/// backend for no longer than that: an output that goes away is dropped by its
/// own `destroy`, and the renderer may die first, after which outputs stop being
/// drawn.
class ScreenManager {
public:
    /// The list to draw on `output`, in layout coordinates: one list for every
    /// output, each showing the part the layout puts it on. The usual thing is
    /// to ignore `output` and return the same list; a compositor that does build
    /// per output must keep its coordinates in the layout's all the same.
    ///
    /// Valid until the builder is next called, and it is called in response to
    /// input as well as for frames, so it should refill the same vector rather
    /// than make a new one: that is what keeps a steady frame allocation-free.
    using SceneBuilder = std::function<std::span<const SceneItem>(Output& output)>;

    ScreenManager(Display& display, Backend& backend, SceneRenderer& renderer, OutputLayout& layout,
                  ScreenOptions options = {});
    ~ScreenManager();
    ScreenManager(ScreenManager&&) noexcept;
    ScreenManager& operator=(ScreenManager&&) noexcept;
    ScreenManager(const ScreenManager&) = delete;
    ScreenManager& operator=(const ScreenManager&) = delete;

    /// Until there is one, outputs show the background.
    void set_scene_builder(SceneBuilder builder);

    /// Draw `popups` the way the scene does: an `invalidate()` whenever one maps,
    /// moves or goes away, and a popup kept on the output its parent is on,
    /// looked up in the scene when the shell asks. Instead of
    /// `XdgShell::set_popup_constraint_query()`.
    void follow_popups(PopupTree& popups);

    /// The list for the output under (`x`, `y`), or for the first output when the
    /// point is in a gap; empty with no outputs. What input routes against.
    [[nodiscard]] std::span<const SceneItem> scene_at(double x, double y);

    /// Where `surface` is in the scene, on whichever output has it.
    [[nodiscard]] std::optional<Box> surface_box(SurfaceId surface);

    /// Something no client reports changed — a window opened, closed, moved or
    /// went up the stack. Asks every output for a frame, and the frame's diff
    /// works out what moved.
    void invalidate();

    /// How many outputs are being managed.
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] Signal<ScreenInvalidated>& invalidated() noexcept;
    [[nodiscard]] Signal<ScreenFailure>& failed() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct ScreenEntry {
    Output* output = nullptr;
    SceneOutput state;
    std::optional<OutputGlobal> global;
    Signal<FrameEvent>::Connection on_frame;
    Signal<OutputDestroy>::Connection on_destroy;
};

struct ScreenManager::Impl {
    Display* display = nullptr;
    SceneRenderer* renderer = nullptr;
    std::weak_ptr<const void> renderer_alive;
    OutputLayout* layout = nullptr;
    ScreenOptions options;
    SceneBuilder builder;
    // A list for the addresses: `frame` and `destroy` hold on to their entry.
    std::list<ScreenEntry> screens;
    std::uint64_t named = 0;
    PopupTree* popups = nullptr;
    Signal<PopupTreeChange>::Connection on_popups;
    Signal<NewOutput>::Connection on_new_output;
    Signal<ScreenInvalidated> invalidated;
    Signal<ScreenFailure> failed;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        // connected() says the tree still exists: it can go first.
        if (on_popups.connected()) {
            popups->clear_constraint();
        }
    }

    void report(Output& output, std::string_view message) {
        if (failed.slot_count() == 0) {
            std::println(std::cerr, "luminaria: {}", message);
            return;
        }
        ScreenFailure failure{output, message};
        failed.emit(failure);
    }

    void add(Output& output);
    void present(ScreenEntry& screen);
    void invalidate();
    [[nodiscard]] std::optional<Box> surface_box(SurfaceId surface);
    [[nodiscard]] std::span<const SceneItem> scene_on(Output& output) {
        return builder ? builder(output) : std::span<const SceneItem>{};
    }
};

void ScreenManager::Impl::add(Output& output) {
    if (renderer_alive.expired()) {
        return;
    }
    ScreenEntry& screen = screens.emplace_back();
    screen.output = &output;
    if (Status ready = renderer->reset(screen.state, output, options.drm_format); !ready) {
        report(output, "render target: " + ready.error().message + "; compositing on the CPU");
    }
    if (options.join_layout) {
        layout->add_auto(output);
    }
    if (options.variable_refresh && output.vrr_capable()) {
        (void)output.set_vrr(true); // a driver that refuses costs VRR, not the output
    }
    if (options.announce) {
        screen.global = to_optional(OutputGlobal::create(
            *display, output, *layout, std::format("{}-{}", options.name, ++named)));
    }
    screen.on_frame = output.frame.connect([this, &screen](FrameEvent&) { present(screen); });
    screen.on_destroy = output.destroy.connect([this, &screen](OutputDestroy&) {
        // A slot may erase itself: a Signal never frees the one that is running.
        std::erase_if(screens, [&screen](const ScreenEntry& x) { return &x == &screen; });
    });
    output.schedule_frame();
}

void ScreenManager::Impl::present(ScreenEntry& screen) {
    if (renderer_alive.expired()) {
        return;
    }
    Output& output = *screen.output;
    Result<SceneOutcome> shown =
        renderer->present(screen.state, output, scene_on(output), options.background, *layout);
    if (!shown) {
        report(output, "present: " + shown.error().message);
    }
}

void ScreenManager::Impl::invalidate() {
    for (const ScreenEntry& screen : screens) {
        screen.output->schedule_frame();
    }
    ScreenInvalidated event;
    invalidated.emit(event);
}

std::optional<Box> ScreenManager::Impl::surface_box(SurfaceId surface) {
    for (const ScreenEntry& screen : screens) {
        const std::span<const SceneItem> scene = scene_on(*screen.output);
        if (const auto item = std::ranges::find(scene, surface, &SceneItem::surface);
            item != scene.end()) {
            return item->box;
        }
    }
    return std::nullopt;
}

ScreenManager::ScreenManager(Display& display, Backend& backend, SceneRenderer& renderer,
                             OutputLayout& layout, ScreenOptions options)
    : impl_(std::make_unique<Impl>()) {
    Impl& self = *impl_;
    self.display = &display;
    self.renderer = &renderer;
    self.renderer_alive = renderer.alive();
    self.layout = &layout;
    self.options = std::move(options);
    self.on_new_output = backend.new_output.connect([&self](NewOutput& e) { self.add(e.output); });
}

ScreenManager::~ScreenManager() = default;
ScreenManager::ScreenManager(ScreenManager&&) noexcept = default;
ScreenManager& ScreenManager::operator=(ScreenManager&&) noexcept = default;

void ScreenManager::set_scene_builder(SceneBuilder builder) {
    impl_->builder = std::move(builder);
    impl_->invalidate();
}

void ScreenManager::follow_popups(PopupTree& popups) {
    Impl& self = *impl_;
    self.popups = &popups;
    self.on_popups = popups.changed.connect([&self] { self.invalidate(); });
    popups.constrain(*self.layout,
                     [&self](SurfaceId surface) { return self.surface_box(surface); });
}

std::span<const SceneItem> ScreenManager::scene_at(double x, double y) {
    Impl& self = *impl_;
    Output* on = self.layout->at(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
    if (on == nullptr && !self.screens.empty()) {
        on = self.screens.front().output;
    }
    return on != nullptr ? self.scene_on(*on) : std::span<const SceneItem>{};
}

std::optional<Box> ScreenManager::surface_box(SurfaceId surface) {
    return impl_->surface_box(surface);
}

void ScreenManager::invalidate() { impl_->invalidate(); }

std::size_t ScreenManager::size() const noexcept { return impl_->screens.size(); }

Signal<ScreenInvalidated>& ScreenManager::invalidated() noexcept { return impl_->invalidated; }

Signal<ScreenFailure>& ScreenManager::failed() noexcept { return impl_->failed; }

} // namespace luminaria
