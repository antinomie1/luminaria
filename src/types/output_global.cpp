#include "luminaria/output_global.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "luminaria/core/display.hpp"

namespace luminaria {

struct OutputGlobal::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    int width = 0;
    int height = 0;

    ~Impl() {
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

void output_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<OutputGlobal::Impl*>(data);
    wl_resource* resource =
        wl_resource_create(client, &wl_output_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_impl, self, nullptr);

    wl_output_send_geometry(resource, 0, 0, self->width, self->height, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "luminaria", "virtual", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, self->width,
                        self->height, 60000);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        wl_output_send_scale(resource, 1);
    }
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(resource);
    }
}

} // namespace

OutputGlobal::OutputGlobal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
OutputGlobal::~OutputGlobal() = default;
OutputGlobal::OutputGlobal(OutputGlobal&&) noexcept = default;
OutputGlobal& OutputGlobal::operator=(OutputGlobal&&) noexcept = default;

Result<OutputGlobal> OutputGlobal::create(Display& display, int width, int height) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->width = width;
    impl->height = height;
    // Version 3: geometry/mode/scale/done + release. Enough for real clients.
    impl->global =
        wl_global_create(impl->display, &wl_output_interface, 3, impl.get(), output_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wl_output) failed");
    }
    return OutputGlobal{std::move(impl)};
}

} // namespace luminaria
