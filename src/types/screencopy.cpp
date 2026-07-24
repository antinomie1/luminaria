// screencopy.cpp — wlr-screencopy-unstable-v1 + ext-image-copy-capture-v1 protocol glue.
//
// Implements both the legacy wlr-screencopy and the newer ext-image-copy-capture
// protocols. Only wl_shm buffers are supported (no dmabuf for now).
// RAII: each session/frame owns its wl_resource; destruction cleanly unregisters.
#include "luminaria/screencopy.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <ctime>

#include <sys/mman.h>
#include <sys/stat.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "wlr-screencopy-unstable-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"
#include "ext-image-capture-source-v1-protocol.h"

#include "luminaria/core/display.hpp"
#include "luminaria/linux_dmabuf.hpp"
#include "luminaria/render/vulkan.hpp"

namespace luminaria {

// =============================================================================
// Helper: get monotonic time as a (tv_sec_hi, tv_sec_lo, tv_nsec) triple
// =============================================================================
namespace {
struct Timespec {
    uint32_t tv_sec_hi;
    uint32_t tv_sec_lo;
    uint32_t tv_nsec;
};
Timespec monotonic_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return {
        static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) >> 32),
        static_cast<uint32_t>(ts.tv_sec & 0xFFFFFFFF),
        static_cast<uint32_t>(ts.tv_nsec),
    };
}
} // namespace

// =============================================================================
// OutputEntry: tracks a capturable output
// =============================================================================
struct OutputEntry {
    wl_resource* output;           // the wl_output resource
    int width;
    int height;
    ScreencopyCaptureFunc capture; // how to get pixels
};

// =============================================================================
// ScreencopyManager::Impl
// =============================================================================
struct ScreencopyManager::Impl {
    wl_display* display = nullptr;
    VulkanRenderer* renderer = nullptr; // set → dmabuf targets advertised/writable
    dev_t render_dev = 0;               // DRM render node dev_t (for ext dmabuf_device)
    bool has_dev = false;

    // wlr-screencopy globals
    wl_global* wlr_manager_global = nullptr;

    // ext-image-copy-capture global
    wl_global* ext_copy_capture_global = nullptr;
    // ext-image-capture-source globals
    wl_global* ext_source_output_manager_global = nullptr;

    std::vector<OutputEntry> outputs;

    // Find an output entry by its wl_resource. Returns nullptr if not found.
    OutputEntry* find_output(wl_resource* output) {
        for (auto& o : outputs) {
            if (o.output == output) {
                return &o;
            }
        }
        return nullptr;
    }

    ~Impl() {
        if (wlr_manager_global) wl_global_destroy(wlr_manager_global);
        if (ext_copy_capture_global) wl_global_destroy(ext_copy_capture_global);
        if (ext_source_output_manager_global) wl_global_destroy(ext_source_output_manager_global);
    }
};

// =============================================================================
// wlr-screencopy-unstable-v1 implementation
// =============================================================================
namespace {

// ---- zwlr_screencopy_frame_v1 ----
struct WlrFrame {
    ScreencopyManager::Impl* mgr = nullptr;
    wl_resource* resource = nullptr;
    OutputEntry* output = nullptr;
    int region_x = 0, region_y = 0, region_w = 0, region_h = 0;
    bool overlay_cursor = false;
    bool copy_requested = false;
    bool copy_with_damage = false; // true if copy_with_damage was used

    ~WlrFrame() {
        // Resource is destroyed by libwayland; just clean up pointers.
    }
};

void wlr_frame_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

/// Write tightly-packed RGBA8 into the shm buffer. The shm buffer has stride
/// `shm_stride` bytes per row, format WL_SHM_FORMAT_ARGB8888.
bool write_shm_rgba(wl_resource* buffer, const std::vector<uint8_t>& rgba,
                    int width, int height, int shm_stride) {
    wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    if (!shm) { return false; }

    wl_shm_buffer_begin_access(shm);
    auto* dst = static_cast<uint8_t*>(wl_shm_buffer_get_data(shm));
    if (!dst) { wl_shm_buffer_end_access(shm); return false; }

    // Copy row by row, converting RGBA → ARGB8888 (little-endian BGRA).
    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = rgba.data() + static_cast<size_t>(y) * width * 4;
        uint8_t* dst_row = dst + static_cast<size_t>(y) * shm_stride;
        for (int x = 0; x < width; ++x) {
            dst_row[x * 4 + 0] = src_row[x * 4 + 2];
            dst_row[x * 4 + 1] = src_row[x * 4 + 1];
            dst_row[x * 4 + 2] = src_row[x * 4 + 0];
            dst_row[x * 4 + 3] = src_row[x * 4 + 3];
        }
    }
    wl_shm_buffer_end_access(shm);
    return true;
}

bool dmabuf_is_linear(uint64_t m) {
    return m == DRM_FORMAT_MOD_LINEAR || m == DRM_FORMAT_MOD_INVALID;
}

/// Write tightly-packed RGBA8 into a client dmabuf target (ARGB8888/XRGB8888).
/// LINEAR is mmap'd; any other modifier goes through the renderer. Returns false
/// on size/format mismatch or a missing renderer for a tiled buffer.
bool write_dmabuf_rgba(VulkanRenderer* renderer, wl_resource* buffer,
                       const std::vector<uint8_t>& rgba, int width, int height) {
    DmabufInfo info;
    if (!dmabuf_buffer_info(buffer, info)) {
        return false;
    }
    if (info.width != width || info.height != height ||
        (info.format != DRM_FORMAT_ARGB8888 && info.format != DRM_FORMAT_XRGB8888)) {
        return false;
    }
    if (dmabuf_is_linear(info.modifier)) {
        const size_t len =
            static_cast<size_t>(info.offset) + static_cast<size_t>(info.stride) * height;
        void* map = mmap(nullptr, len, PROT_WRITE, MAP_SHARED, info.fd, 0);
        if (map == MAP_FAILED) {
            return false;
        }
        auto* base = static_cast<uint8_t*>(map) + info.offset;
        for (int y = 0; y < height; ++y) {
            const uint8_t* src = rgba.data() + static_cast<size_t>(y) * width * 4;
            uint8_t* dst = base + static_cast<size_t>(y) * info.stride;
            for (int x = 0; x < width; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2]; // B
                dst[x * 4 + 1] = src[x * 4 + 1]; // G
                dst[x * 4 + 2] = src[x * 4 + 0]; // R
                dst[x * 4 + 3] = src[x * 4 + 3]; // A
            }
        }
        munmap(map, len);
        return true;
    }
    if (renderer == nullptr) {
        return false;
    }
    return renderer
        ->export_dmabuf(info.fd, width, height, info.format, info.offset, info.stride, info.modifier,
                        rgba)
        .has_value();
}

void wlr_frame_do_copy(WlrFrame* f, wl_resource* buffer, bool with_damage) {
    if (f->copy_requested) {
        wl_resource_post_error(f->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED,
                               "frame already used for copy");
        return;
    }
    f->copy_requested = true;
    f->copy_with_damage = with_damage;

    const int cap_w = f->region_w;
    const int cap_h = f->region_h;

    // Validate the target buffer (shm or dmabuf) before capturing.
    wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    int shm_stride = 0;
    if (shm) {
        const int bw = wl_shm_buffer_get_width(shm);
        const int bh = wl_shm_buffer_get_height(shm);
        const uint32_t fmt = wl_shm_buffer_get_format(shm);
        shm_stride = wl_shm_buffer_get_stride(shm);
        if (bw != cap_w || bh != cap_h || fmt != WL_SHM_FORMAT_ARGB8888) {
            wl_resource_post_error(
                f->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
                "buffer size/format mismatch: expected %dx%d ARGB8888, got %dx%d fmt=%u", cap_w,
                cap_h, bw, bh, fmt);
            return;
        }
    } else {
        DmabufInfo info;
        if (!dmabuf_buffer_info(buffer, info)) {
            zwlr_screencopy_frame_v1_send_failed(f->resource);
            return;
        }
        if (info.width != cap_w || info.height != cap_h) {
            wl_resource_post_error(f->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
                                   "dmabuf size mismatch: expected %dx%d, got %dx%d", cap_w, cap_h,
                                   info.width, info.height);
            return;
        }
    }

    // Capture pixels from the compositor.
    std::vector<uint8_t> rgba;
    if (!f->output->capture(f->region_x, f->region_y, cap_w, cap_h, rgba)) {
        zwlr_screencopy_frame_v1_send_failed(f->resource);
        return;
    }

    const bool wrote = shm ? write_shm_rgba(buffer, rgba, cap_w, cap_h, shm_stride)
                           : write_dmabuf_rgba(f->mgr->renderer, buffer, rgba, cap_w, cap_h);
    if (!wrote) {
        zwlr_screencopy_frame_v1_send_failed(f->resource);
        return;
    }

    // Send damage (v2+) for copy_with_damage: full damage since we capture the whole region.
    if (with_damage && wl_resource_get_version(f->resource) >= 2) {
        zwlr_screencopy_frame_v1_send_damage(f->resource, 0, 0,
                                               static_cast<uint32_t>(cap_w),
                                               static_cast<uint32_t>(cap_h));
    }

    uint32_t flags = 0;
    zwlr_screencopy_frame_v1_send_flags(f->resource, flags);

    auto ts = monotonic_time();
    zwlr_screencopy_frame_v1_send_ready(f->resource, ts.tv_sec_hi, ts.tv_sec_lo, ts.tv_nsec);
}

void wlr_frame_copy(wl_client*, wl_resource* resource, wl_resource* buffer) {
    auto* f = static_cast<WlrFrame*>(wl_resource_get_user_data(resource));
    wlr_frame_do_copy(f, buffer, false);
}

void wlr_frame_copy_with_damage(wl_client*, wl_resource* resource, wl_resource* buffer) {
    auto* f = static_cast<WlrFrame*>(wl_resource_get_user_data(resource));
    wlr_frame_do_copy(f, buffer, true);
}

constexpr struct zwlr_screencopy_frame_v1_interface wlr_frame_impl = {
    .copy = wlr_frame_copy,
    .destroy = wlr_frame_destroy_request,
    .copy_with_damage = wlr_frame_copy_with_damage,
};

void wlr_frame_resource_destroy(wl_resource* resource) {
    auto* f = static_cast<WlrFrame*>(wl_resource_get_user_data(resource));
    delete f;
}

// ---- zwlr_screencopy_manager_v1 ----
ScreencopyManager::Impl* wlr_manager_of(wl_resource* r) {
    return static_cast<ScreencopyManager::Impl*>(wl_resource_get_user_data(r));
}

void wlr_manager_capture_output(wl_client* client, wl_resource* mgr_resource, uint32_t id,
                                 int32_t overlay_cursor, wl_resource* output) {
    auto* mgr = wlr_manager_of(mgr_resource);
    auto* out = mgr->find_output(output);
    if (!out) {
        // Output not registered; we still create the frame but will fail on copy.
        // Actually, let's just send failed immediately.
        wl_resource_post_error(mgr_resource, 0, "unknown output");
        return;
    }

    uint32_t version = wl_resource_get_version(mgr_resource);
    wl_resource* frame_res = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface,
                                                  static_cast<int>(version), id);
    if (!frame_res) {
        wl_client_post_no_memory(client);
        return;
    }

    auto* frame = new WlrFrame{mgr, frame_res, out, 0, 0, out->width, out->height,
                                 overlay_cursor != 0, false, false};
    wl_resource_set_implementation(frame_res, &wlr_frame_impl, frame, wlr_frame_resource_destroy);

    // Send buffer info: we always support ARGB8888.
    zwlr_screencopy_frame_v1_send_buffer(frame_res, WL_SHM_FORMAT_ARGB8888,
                                           static_cast<uint32_t>(out->width),
                                           static_cast<uint32_t>(out->height),
                                           static_cast<uint32_t>(out->width * 4));
    // v3: advertise dmabuf targets (if enabled), then end enumeration.
    if (version >= 3) {
        if (mgr->renderer) {
            zwlr_screencopy_frame_v1_send_linux_dmabuf(frame_res, DRM_FORMAT_XRGB8888,
                                                       static_cast<uint32_t>(out->width),
                                                       static_cast<uint32_t>(out->height));
            zwlr_screencopy_frame_v1_send_linux_dmabuf(frame_res, DRM_FORMAT_ARGB8888,
                                                       static_cast<uint32_t>(out->width),
                                                       static_cast<uint32_t>(out->height));
        }
        zwlr_screencopy_frame_v1_send_buffer_done(frame_res);
    }
}

void wlr_manager_capture_output_region(wl_client* client, wl_resource* mgr_resource, uint32_t id,
                                        int32_t overlay_cursor, wl_resource* output,
                                        int32_t x, int32_t y, int32_t width, int32_t height) {
    auto* mgr = wlr_manager_of(mgr_resource);
    auto* out = mgr->find_output(output);
    if (!out) {
        wl_resource_post_error(mgr_resource, 0, "unknown output");
        return;
    }

    // Clip to output bounds.
    int cx = std::max(0, x);
    int cy = std::max(0, y);
    int cw = std::clamp(width, 1, out->width - cx);
    int ch = std::clamp(height, 1, out->height - cy);

    uint32_t version = wl_resource_get_version(mgr_resource);
    wl_resource* frame_res = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface,
                                                  static_cast<int>(version), id);
    if (!frame_res) {
        wl_client_post_no_memory(client);
        return;
    }

    auto* frame = new WlrFrame{mgr, frame_res, out, cx, cy, cw, ch,
                                 overlay_cursor != 0, false, false};
    wl_resource_set_implementation(frame_res, &wlr_frame_impl, frame, wlr_frame_resource_destroy);

    zwlr_screencopy_frame_v1_send_buffer(frame_res, WL_SHM_FORMAT_ARGB8888,
                                           static_cast<uint32_t>(cw),
                                           static_cast<uint32_t>(ch),
                                           static_cast<uint32_t>(cw * 4));
    if (version >= 3) {
        if (mgr->renderer) {
            zwlr_screencopy_frame_v1_send_linux_dmabuf(frame_res, DRM_FORMAT_XRGB8888,
                                                       static_cast<uint32_t>(cw),
                                                       static_cast<uint32_t>(ch));
            zwlr_screencopy_frame_v1_send_linux_dmabuf(frame_res, DRM_FORMAT_ARGB8888,
                                                       static_cast<uint32_t>(cw),
                                                       static_cast<uint32_t>(ch));
        }
        zwlr_screencopy_frame_v1_send_buffer_done(frame_res);
    }
}

void wlr_manager_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct zwlr_screencopy_manager_v1_interface wlr_manager_impl = {
    .capture_output = wlr_manager_capture_output,
    .capture_output_region = wlr_manager_capture_output_region,
    .destroy = wlr_manager_destroy,
};

void wlr_manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwlr_screencopy_manager_v1_interface,
                                                 static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &wlr_manager_impl, data, nullptr);
}

} // namespace

// =============================================================================
// ext-image-copy-capture-v1 implementation
// =============================================================================
namespace {

// ---- ext_image_copy_capture_frame_v1 ----
struct ExtFrame {
    ScreencopyManager::Impl* mgr = nullptr;
    wl_resource* resource = nullptr;
    OutputEntry* output = nullptr;
    wl_resource* attached_buffer = nullptr;
    bool captured = false;
    // attach_buffer and capture are separate requests, so the buffer is held
    // across a round trip — and a client may destroy it in between. Same rule as
    // wl_surface: never keep a raw buffer resource without a destroy listener.
    wl_listener buffer_destroy{};

    ~ExtFrame() {
        if (buffer_destroy.link.prev != nullptr) {
            wl_list_remove(&buffer_destroy.link);
        }
    }
};

void ext_frame_forget_buffer(wl_listener* listener, void*) {
    auto* f = reinterpret_cast<ExtFrame*>(reinterpret_cast<char*>(listener) -
                                          offsetof(ExtFrame, buffer_destroy));
    f->attached_buffer = nullptr;
    wl_list_init(&f->buffer_destroy.link); // libwayland already unlinked it
}

void ext_frame_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void ext_frame_attach_buffer(wl_client*, wl_resource* resource, wl_resource* buffer) {
    auto* f = static_cast<ExtFrame*>(wl_resource_get_user_data(resource));
    if (f->captured) {
        wl_resource_post_error(f->resource,
                               EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "frame already captured");
        return;
    }
    if (f->attached_buffer != nullptr && f->buffer_destroy.link.prev != nullptr) {
        wl_list_remove(&f->buffer_destroy.link);
        wl_list_init(&f->buffer_destroy.link);
    }
    f->attached_buffer = buffer;
    if (buffer != nullptr) {
        f->buffer_destroy.notify = ext_frame_forget_buffer;
        wl_resource_add_destroy_listener(buffer, &f->buffer_destroy);
    }
}

void ext_frame_damage_buffer(wl_client*, wl_resource* resource,
                              int32_t, int32_t, int32_t, int32_t) {
    auto* f = static_cast<ExtFrame*>(wl_resource_get_user_data(resource));
    if (f->captured) {
        wl_resource_post_error(f->resource,
                               EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "frame already captured");
        return;
    }
    // We ignore damage for now and always capture the full region.
}

void ext_frame_capture(wl_client*, wl_resource* resource) {
    auto* f = static_cast<ExtFrame*>(wl_resource_get_user_data(resource));
    if (f->captured) {
        wl_resource_post_error(f->resource,
                               EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "frame already captured");
        return;
    }
    if (!f->attached_buffer) {
        wl_resource_post_error(f->resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
                               "no buffer attached");
        return;
    }
    f->captured = true;

    // Capture pixels.
    std::vector<uint8_t> rgba;
    if (!f->output->capture(0, 0, f->output->width, f->output->height, rgba)) {
        ext_image_copy_capture_frame_v1_send_failed(f->resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }

    wl_shm_buffer* shm = wl_shm_buffer_get(f->attached_buffer);
    bool wrote = false;
    if (shm) {
        int stride = wl_shm_buffer_get_stride(shm);
        wrote = write_shm_rgba(f->attached_buffer, rgba, f->output->width, f->output->height, stride);
    } else {
        wrote = write_dmabuf_rgba(f->mgr->renderer, f->attached_buffer, rgba, f->output->width,
                                  f->output->height);
    }
    if (!wrote) {
        ext_image_copy_capture_frame_v1_send_failed(f->resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        return;
    }

    ext_image_copy_capture_frame_v1_send_transform(f->resource, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_damage(f->resource, 0, 0,
                                                  static_cast<int32_t>(f->output->width),
                                                  static_cast<int32_t>(f->output->height));
    auto ts = monotonic_time();
    ext_image_copy_capture_frame_v1_send_presentation_time(f->resource, ts.tv_sec_hi,
                                                             ts.tv_sec_lo, ts.tv_nsec);
    ext_image_copy_capture_frame_v1_send_ready(f->resource);
}

constexpr struct ext_image_copy_capture_frame_v1_interface ext_frame_impl = {
    .destroy = ext_frame_destroy_request,
    .attach_buffer = ext_frame_attach_buffer,
    .damage_buffer = ext_frame_damage_buffer,
    .capture = ext_frame_capture,
};

void ext_frame_resource_destroy(wl_resource* resource) {
    delete static_cast<ExtFrame*>(wl_resource_get_user_data(resource));
}

// ---- ext_image_copy_capture_session_v1 ----
struct ExtSession {
    ScreencopyManager::Impl* mgr = nullptr;
    wl_resource* resource = nullptr;
    OutputEntry* output = nullptr;
    bool stopped = false;
};

void ext_session_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void ext_session_create_frame(wl_client* client, wl_resource* session_resource, uint32_t id) {
    auto* s = static_cast<ExtSession*>(wl_resource_get_user_data(session_resource));
    if (s->stopped) return;

    uint32_t version = wl_resource_get_version(session_resource);
    wl_resource* frame_res = wl_resource_create(client,
                                                  &ext_image_copy_capture_frame_v1_interface,
                                                  static_cast<int>(version), id);
    if (!frame_res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* frame = new ExtFrame{};
    frame->mgr = s->mgr;
    frame->resource = frame_res;
    frame->output = s->output;
    wl_resource_set_implementation(frame_res, &ext_frame_impl, frame, ext_frame_resource_destroy);
}

constexpr struct ext_image_copy_capture_session_v1_interface ext_session_impl = {
    .create_frame = ext_session_create_frame,
    .destroy = ext_session_destroy_request,
};

void ext_session_resource_destroy(wl_resource* resource) {
    delete static_cast<ExtSession*>(wl_resource_get_user_data(resource));
}

// ---- ext_image_copy_capture_manager_v1 ----
ScreencopyManager::Impl* ext_manager_of(wl_resource* r) {
    return static_cast<ScreencopyManager::Impl*>(wl_resource_get_user_data(r));
}

void ext_manager_create_session(wl_client* client, wl_resource* mgr_resource, uint32_t id,
                                 wl_resource* source, uint32_t options) {
    (void)options; // paint_cursors flag; not implemented yet
    auto* mgr = ext_manager_of(mgr_resource);
    // Find which output this source belongs to. We store the source -> output
    // mapping in source user_data.
    auto* out = static_cast<OutputEntry*>(wl_resource_get_user_data(source));
    if (!out) {
        wl_resource_post_error(mgr_resource, 0, "invalid source");
        return;
    }

    wl_resource* session_res = wl_resource_create(client,
                                                    &ext_image_copy_capture_session_v1_interface,
                                                    1, id);
    if (!session_res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* session = new ExtSession{mgr, session_res, out, false};
    wl_resource_set_implementation(session_res, &ext_session_impl, session,
                                     ext_session_resource_destroy);

    // Send buffer constraints: full output size, ARGB8888 shm, plus dmabuf
    // (device + per-format modifiers) when a renderer is enabled.
    ext_image_copy_capture_session_v1_send_buffer_size(session_res,
        static_cast<uint32_t>(out->width), static_cast<uint32_t>(out->height));
    ext_image_copy_capture_session_v1_send_shm_format(session_res, WL_SHM_FORMAT_ARGB8888);
    if (mgr->renderer && mgr->has_dev) {
        struct wl_array dev;
        wl_array_init(&dev);
        std::memcpy(wl_array_add(&dev, sizeof(dev_t)), &mgr->render_dev, sizeof(dev_t));
        ext_image_copy_capture_session_v1_send_dmabuf_device(session_res, &dev);
        wl_array_release(&dev);

        for (uint32_t fmt : {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888}) {
            auto mods = mgr->renderer->dmabuf_modifiers(fmt);
            if (mods.empty()) {
                mods.push_back(DRM_FORMAT_MOD_LINEAR);
            }
            struct wl_array ma;
            wl_array_init(&ma);
            for (uint64_t m : mods) {
                std::memcpy(wl_array_add(&ma, sizeof(uint64_t)), &m, sizeof(uint64_t));
            }
            ext_image_copy_capture_session_v1_send_dmabuf_format(session_res, fmt, &ma);
            wl_array_release(&ma);
        }
    }
    ext_image_copy_capture_session_v1_send_done(session_res);
}

// We don't support pointer cursor capture for now.
void ext_manager_create_pointer_cursor_session(wl_client*, wl_resource*, uint32_t, wl_resource*,
                                                 wl_resource*) {
    // no-op: unsupported
}

void ext_manager_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct ext_image_copy_capture_manager_v1_interface ext_manager_impl = {
    .create_session = ext_manager_create_session,
    .create_pointer_cursor_session = ext_manager_create_pointer_cursor_session,
    .destroy = ext_manager_destroy,
};

void ext_manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client,
                                                 &ext_image_copy_capture_manager_v1_interface,
                                                 static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &ext_manager_impl, data, nullptr);
}

// ---- ext_output_image_capture_source_manager_v1 ----
ScreencopyManager::Impl* ext_source_manager_of(wl_resource* r) {
    return static_cast<ScreencopyManager::Impl*>(wl_resource_get_user_data(r));
}

void ext_source_manager_create_source(wl_client* client, wl_resource* mgr_resource, uint32_t id,
                                       wl_resource* output) {
    auto* mgr = ext_source_manager_of(mgr_resource);
    auto* out = mgr->find_output(output);
    if (!out) {
        wl_resource_post_error(mgr_resource, 0, "unknown output");
        return;
    }

    wl_resource* source_res = wl_resource_create(client,
                                                   &ext_image_capture_source_v1_interface, 1, id);
    if (!source_res) {
        wl_client_post_no_memory(client);
        return;
    }
    // Store the OutputEntry pointer as user_data so the copy-capture manager
    // can resolve it later. Provide a minimal implementation so destroy works.
    static constexpr struct ext_image_capture_source_v1_interface source_impl = {
        .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    };
    wl_resource_set_implementation(source_res, &source_impl, out, nullptr);
}

void ext_source_manager_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct ext_output_image_capture_source_manager_v1_interface ext_source_manager_impl = {
    .create_source = ext_source_manager_create_source,
    .destroy = ext_source_manager_destroy,
};

void ext_source_manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client,
        &ext_output_image_capture_source_manager_v1_interface,
        static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &ext_source_manager_impl, data, nullptr);
}

} // namespace

// =============================================================================
// ScreencopyManager public API
// =============================================================================

ScreencopyManager::ScreencopyManager(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ScreencopyManager::~ScreencopyManager() = default;
ScreencopyManager::ScreencopyManager(ScreencopyManager&&) noexcept = default;
ScreencopyManager& ScreencopyManager::operator=(ScreencopyManager&&) noexcept = default;

Result<ScreencopyManager> ScreencopyManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();

    // wlr-screencopy-unstable-v1 (version 3: shm + dmabuf info, but we only
    // actually support shm for now).
    impl->wlr_manager_global = wl_global_create(impl->display,
                                                  &zwlr_screencopy_manager_v1_interface, 3,
                                                  impl.get(), wlr_manager_bind);
    if (!impl->wlr_manager_global) {
        return fail("wl_global_create(zwlr_screencopy_manager_v1) failed");
    }

    // ext-image-copy-capture-v1
    impl->ext_copy_capture_global = wl_global_create(impl->display,
                                                       &ext_image_copy_capture_manager_v1_interface,
                                                       1, impl.get(), ext_manager_bind);
    if (!impl->ext_copy_capture_global) {
        return fail("wl_global_create(ext_image_copy_capture_manager_v1) failed");
    }

    // ext-output-image-capture-source-manager-v1
    impl->ext_source_output_manager_global = wl_global_create(
        impl->display, &ext_output_image_capture_source_manager_v1_interface,
        1, impl.get(), ext_source_manager_bind);
    if (!impl->ext_source_output_manager_global) {
        return fail("wl_global_create(ext_output_image_capture_source_manager_v1) failed");
    }

    return ScreencopyManager{std::move(impl)};
}

void ScreencopyManager::add_output(wl_resource* output, int width, int height,
                                     ScreencopyCaptureFunc capture) {
    impl_->outputs.push_back(OutputEntry{output, width, height, std::move(capture)});
}

void ScreencopyManager::set_renderer(VulkanRenderer* renderer) {
    impl_->renderer = (renderer != nullptr && renderer->dmabuf_supported()) ? renderer : nullptr;
    // dev_t of the render node clients should allocate dmabufs on (ext protocol).
    struct stat st;
    if (impl_->renderer != nullptr && stat("/dev/dri/renderD128", &st) == 0) {
        impl_->render_dev = st.st_rdev;
        impl_->has_dev = true;
    } else {
        impl_->has_dev = false;
    }
}

} // namespace luminaria
