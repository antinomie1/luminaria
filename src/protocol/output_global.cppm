// luminaria/output_global.cppm — the wl_output global.
//
// This is the protocol object clients read to learn there's a display and how
// big it is. Distinct from the backend `Output` (which owns scanout): many
// clients (e.g. weston-terminal) won't map a window until they see a wl_output.
//
// Importing luminaria pulls in no libwayland headers: the C types this
// interface names are forward-declared in the global module fragment.
//
// It also carries the matching `zxdg_output_manager_v1` (xdg-output-unstable-v1):
// wl_output only describes the physical mode, so tools that arrange outputs in a
// layout — screenshot utilities above all — read the *logical* position and size
// from xdg-output instead. `grim` warns and produces a zero-sized image without it.

module;

#include "detail/wayland_fwd.h"
#include <functional>
#include <typeinfo>
#include <memory>
#include <string>

#include <vector>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "xdg-output-unstable-v1-protocol.h"

export module luminaria:output_global;

import :display;
import :expected;
import :output;
import :transform;

export namespace luminaria {

class Display;

/// A single wl_output advertising a fixed mode. Move-only; pointer-stable state
/// so the libwayland global can hold a pointer to it.
class OutputGlobal {
public:
    /// Advertise an output of `width`x`height` px at 60Hz, scale 1, under
    /// `name` (what `grim -o` and other tools address it by).
    [[nodiscard]] static Result<OutputGlobal> create(Display& display, int width, int height,
                                                     std::string name = "luminaria-1");

    ~OutputGlobal();
    OutputGlobal(OutputGlobal&&) noexcept;
    OutputGlobal& operator=(OutputGlobal&&) noexcept;
    OutputGlobal(const OutputGlobal&) = delete;
    OutputGlobal& operator=(const OutputGlobal&) = delete;

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;

    /// Register a callback invoked for every wl_output resource created
    /// (past and future). Used by screencopy to track capturable outputs.
    using BindFunc = std::function<void(wl_resource*)>;
    void on_bind(BindFunc fn);

    /// This output's wl_output resource for `client`, or null if that client
    /// never bound it. Protocols that reference an output in an event
    /// (ext-workspace, presentation-time) can only name a client's own object.
    [[nodiscard]] wl_resource* resource_for(wl_client* client) const;

    /// Where this output sits in the layout, as reported by xdg-output. Set it
    /// from an OutputLayout so tools that arrange screenshots get real numbers.
    void set_logical_position(int x, int y);

    /// Every mode this display can be driven at, so clients (and a display
    /// settings panel) can see what is on offer. Optional: with no list, only
    /// the current mode is advertised, flagged preferred.
    void set_modes(std::vector<OutputMode> modes);

    /// The mode changed — a different resolution or refresh rate is now
    /// driving the display. Re-sends geometry and the mode list to every bound
    /// client, so `wl_output` never describes a mode that is no longer on.
    /// Wire it to `Output::mode_changed`.
    void set_mode(int width, int height, int refresh_mhz = 0);

    /// Integer output scale (wl_output.scale). A HiDPI client renders at this
    /// scale and the compositor no longer has to upscale a blurry buffer.
    void set_scale(int scale);
    /// Rotation/reflection of this output (wl_output.geometry transform).
    void set_transform(Transform transform);

    [[nodiscard]] int scale() const noexcept;
    [[nodiscard]] Transform transform() const noexcept;

    /// Size in layout coordinates: the mode divided by the scale, with the axes
    /// swapped when the transform rotates. This is what xdg-output reports.
    [[nodiscard]] int logical_width() const noexcept;
    [[nodiscard]] int logical_height() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit OutputGlobal(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct OutputGlobal::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    wl_global* xdg_output_global = nullptr;
    int width = 0;
    int height = 0;
    int refresh_mhz = 60000;      // 60Hz unless the backend says otherwise
    std::vector<OutputMode> modes; // empty: advertise only the current one
    int logical_x = 0;
    int logical_y = 0;
    int scale = 1;
    Transform transform = Transform::normal;
    std::string name;
    std::vector<BindFunc> bind_callbacks;
    std::vector<wl_resource*> resources;     // all bound wl_output resources (weak refs)
    std::vector<wl_resource*> xdg_resources; // …and their zxdg_output_v1 companions

    // Layout size: the mode divided by the scale, axes swapped when rotated.
    [[nodiscard]] int logical_width() const {
        return (transform_swaps_axes(transform) ? height : width) / scale;
    }
    [[nodiscard]] int logical_height() const {
        return (transform_swaps_axes(transform) ? width : height) / scale;
    }

    ~Impl() {
        if (xdg_output_global != nullptr) {
            wl_global_destroy(xdg_output_global);
        }
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

namespace {

void output_release(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource); // wl_output.release (v3+)
}
constexpr struct wl_output_interface output_impl = {.release = output_release};

// A client can unbind a wl_output at any time; drop the weak reference or the
// next iteration over Impl::resources walks freed memory.
void output_resource_destroy(wl_resource* resource) {
    auto* self = static_cast<OutputGlobal::Impl*>(wl_resource_get_user_data(resource));
    std::erase(self->resources, resource);
}

// wl_output.mode is sent once per mode, and exactly one of them carries
// CURRENT. With no list from the backend, the current mode is all there is —
// and it is then also the preferred one, because nothing else is on offer.
void send_modes(OutputGlobal::Impl& impl, wl_resource* resource) {
    bool sent_current = false;
    for (const OutputMode& m : impl.modes) {
        uint32_t flags = m.preferred ? WL_OUTPUT_MODE_PREFERRED : 0u;
        if (!sent_current && m.width == impl.width && m.height == impl.height &&
            m.refresh_mhz == impl.refresh_mhz) {
            flags |= WL_OUTPUT_MODE_CURRENT;
            sent_current = true;
        }
        wl_output_send_mode(resource, flags, m.width, m.height, m.refresh_mhz);
    }
    if (!sent_current) {
        wl_output_send_mode(resource,
                            WL_OUTPUT_MODE_CURRENT |
                                (impl.modes.empty() ? WL_OUTPUT_MODE_PREFERRED : 0u),
                            impl.width, impl.height, impl.refresh_mhz);
    }
}

void output_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<OutputGlobal::Impl*>(data);
    wl_resource* resource =
        wl_resource_create(client, &wl_output_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_impl, self, output_resource_destroy);
    // Keep a weak reference so callbacks can iterate all bound resources; the
    // destroy handler above removes it again.
    self->resources.push_back(resource);

    wl_output_send_geometry(resource, 0, 0, self->width, self->height, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "luminaria", "virtual",
                            static_cast<int32_t>(self->transform));
    send_modes(*self, resource);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        wl_output_send_scale(resource, self->scale);
    }
    if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
        wl_output_send_name(resource, self->name.c_str());
        wl_output_send_description(resource, "luminaria virtual output");
    }
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(resource);
    }

    // Notify all registered callbacks (e.g. screencopy manager).
    for (auto& fn : self->bind_callbacks) {
        fn(resource);
    }
}

// ---- xdg-output-unstable-v1 --------------------------------------------------
//
// wl_output describes the physical mode; xdg_output describes where the output
// sits in the compositor's LOGICAL layout and how big it is there. Tools that
// have to place captures on a canvas — grim, slurp, screen recorders — read this
// one. Without it grim guesses, gets nothing, and writes a 0x0 PNG.
//
// Logical position comes from the compositor's OutputLayout; logical size is the
// mode after scale and rotation.

void xdg_output_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void xdg_output_resource_destroy(wl_resource* resource) {
    auto* self = static_cast<OutputGlobal::Impl*>(wl_resource_get_user_data(resource));
    std::erase(self->xdg_resources, resource);
}
constexpr struct zxdg_output_v1_interface xdg_output_impl = {
    .destroy = xdg_output_destroy_request,
};

void xdg_output_manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void xdg_output_manager_get_xdg_output(wl_client* client, wl_resource* manager_resource,
                                       uint32_t id, wl_resource* /*output*/) {
    auto* self = static_cast<OutputGlobal::Impl*>(wl_resource_get_user_data(manager_resource));
    const uint32_t version = static_cast<uint32_t>(wl_resource_get_version(manager_resource));
    wl_resource* resource =
        wl_resource_create(client, &zxdg_output_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xdg_output_impl, self, xdg_output_resource_destroy);
    self->xdg_resources.push_back(resource);

    zxdg_output_v1_send_logical_position(resource, self->logical_x, self->logical_y);
    zxdg_output_v1_send_logical_size(resource, self->logical_width(), self->logical_height());
    if (version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
        zxdg_output_v1_send_name(resource, self->name.c_str());
        zxdg_output_v1_send_description(resource, "luminaria virtual output");
    }
    // Since v3 the atomicity barrier is wl_output.done, not our own done event.
    if (version < ZXDG_OUTPUT_V1_NAME_SINCE_VERSION + 1) {
        zxdg_output_v1_send_done(resource);
    }
}

constexpr struct zxdg_output_manager_v1_interface xdg_output_manager_impl = {
    .destroy = xdg_output_manager_destroy_request,
    .get_xdg_output = xdg_output_manager_get_xdg_output,
};

void xdg_output_manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zxdg_output_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xdg_output_manager_impl, data, nullptr);
}

} // namespace

OutputGlobal::OutputGlobal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
OutputGlobal::~OutputGlobal() = default;
OutputGlobal::OutputGlobal(OutputGlobal&&) noexcept = default;
OutputGlobal& OutputGlobal::operator=(OutputGlobal&&) noexcept = default;

Result<OutputGlobal> OutputGlobal::create(Display& display, int width, int height,
                                         std::string name) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->width = width;
    impl->height = height;
    impl->name = std::move(name);
    // Version 4: geometry/mode/scale/done + name/description (what `grim -o` and
    // other tools address an output by) + release.
    impl->global =
        wl_global_create(impl->display, &wl_output_interface, 4, impl.get(), output_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wl_output) failed");
    }
    impl->xdg_output_global = wl_global_create(impl->display, &zxdg_output_manager_v1_interface, 3,
                                               impl.get(), xdg_output_manager_bind);
    if (impl->xdg_output_global == nullptr) {
        return fail("wl_global_create(zxdg_output_manager_v1) failed");
    }
    return OutputGlobal{std::move(impl)};
}

wl_resource* OutputGlobal::resource_for(wl_client* client) const {
    for (wl_resource* r : impl_->resources) {
        if (wl_resource_get_client(r) == client) {
            return r;
        }
    }
    return nullptr;
}

namespace {

// Re-announce everything the clients already saw and close the atomic update.
// wl_output.done is the barrier for xdg_output too, since xdg-output v3.
void broadcast(OutputGlobal::Impl& impl) {
    for (wl_resource* r : impl.xdg_resources) {
        zxdg_output_v1_send_logical_position(r, impl.logical_x, impl.logical_y);
        zxdg_output_v1_send_logical_size(r, impl.logical_width(), impl.logical_height());
    }
    for (wl_resource* r : impl.resources) {
        const auto version = static_cast<uint32_t>(wl_resource_get_version(r));
        wl_output_send_geometry(r, 0, 0, impl.width, impl.height, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                                "luminaria", "virtual", static_cast<int32_t>(impl.transform));
        send_modes(impl, r);
        if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
            wl_output_send_scale(r, impl.scale);
        }
        if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
            wl_output_send_done(r);
        }
    }
}

} // namespace

void OutputGlobal::set_logical_position(int x, int y) {
    impl_->logical_x = x;
    impl_->logical_y = y;
    broadcast(*impl_);
}

void OutputGlobal::set_modes(std::vector<OutputMode> modes) {
    impl_->modes = std::move(modes);
    broadcast(*impl_);
}

void OutputGlobal::set_mode(int width, int height, int refresh_mhz) {
    impl_->width = width;
    impl_->height = height;
    if (refresh_mhz > 0) {
        impl_->refresh_mhz = refresh_mhz;
    }
    broadcast(*impl_);
}

void OutputGlobal::set_scale(int scale) {
    impl_->scale = scale < 1 ? 1 : scale;
    broadcast(*impl_);
}

void OutputGlobal::set_transform(Transform transform) {
    impl_->transform = transform;
    broadcast(*impl_);
}

int OutputGlobal::scale() const noexcept { return impl_->scale; }
Transform OutputGlobal::transform() const noexcept { return impl_->transform; }
int OutputGlobal::logical_width() const noexcept { return impl_->logical_width(); }
int OutputGlobal::logical_height() const noexcept { return impl_->logical_height(); }

int OutputGlobal::width() const noexcept { return impl_->width; }
int OutputGlobal::height() const noexcept { return impl_->height; }
const std::string& OutputGlobal::name() const noexcept { return impl_->name; }

void OutputGlobal::on_bind(BindFunc fn) {
    // Call for all already-bound resources.
    for (auto* r : impl_->resources) {
        fn(r);
    }
    // Store for future binds.
    impl_->bind_callbacks.push_back(std::move(fn));
}

} // namespace luminaria
