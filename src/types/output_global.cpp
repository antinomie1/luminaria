#include "luminaria/output_global.hpp"

#include <string>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "xdg-output-unstable-v1-protocol.h"

#include "luminaria/core/display.hpp"

namespace luminaria {

struct OutputGlobal::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    wl_global* xdg_output_global = nullptr;
    int width = 0;
    int height = 0;
    std::string name;
    std::vector<BindFunc> bind_callbacks;
    std::vector<wl_resource*> resources; // all bound wl_output resources (weak refs)

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

[[maybe_unused]] void output_resource_destroy(wl_resource*) {} // no-op; resource tracked in Impl::resources

void output_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<OutputGlobal::Impl*>(data);
    wl_resource* resource =
        wl_resource_create(client, &wl_output_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_impl, self, output_resource_destroy);
    // Keep a weak reference; the destroy handler is a no-op but we still store it
    // so callbacks can iterate all bound resources.
    self->resources.push_back(resource);

    wl_output_send_geometry(resource, 0, 0, self->width, self->height, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "luminaria", "virtual", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, self->width,
                        self->height, 60000);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        wl_output_send_scale(resource, 1);
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
// Single fixed output at the origin, so logical == physical.

void xdg_output_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
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
    wl_resource_set_implementation(resource, &xdg_output_impl, self, nullptr);

    zxdg_output_v1_send_logical_position(resource, 0, 0);
    zxdg_output_v1_send_logical_size(resource, self->width, self->height);
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
