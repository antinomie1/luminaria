// luminaria/screencopy.cppm — screen capture protocol (wlr-screencopy-unstable-v1 +
// ext-image-copy-capture-v1). Lets clients capture outputs into wl_shm buffers.
//
// RAII design: ScreencopyManager owns the global; OutputEntry tracks each
// capturable output. When a client requests a frame, a FrameSession is created
// that owns the protocol resource and drives the capture lifecycle.
//
// Thread-safety: all calls must happen on the Wayland event loop thread.

module;

#include "detail/wayland_fwd.h"
#include <cstdint>
#include <typeinfo>

#include <cstddef>
#include <sys/mman.h>
#include <sys/stat.h>
#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "wlr-screencopy-unstable-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"
#include "ext-image-capture-source-v1-protocol.h"

export module luminaria.render:screencopy;

import std;

import luminaria;
import :cpu_compositor;
import :linux_dmabuf;
import :vulkan;

export namespace luminaria {

/// Callback to capture pixels from an output region.
/// Fill `rgba` with tightly-packed RGBA8, row-major, top-to-bottom.
/// Return true on success. Called when a client's copy request arrives.
using ScreencopyCaptureFunc = std::function<bool(int x, int y, int w, int h,
                                                  std::vector<std::uint8_t>& rgba)>;

/// The pointer cursor as it looks right now. A screen recorder that wants the
/// cursor as a separate layer — so it can be composited at the viewer's own
/// frame rate rather than baked into the video — captures it through
/// ext-image-copy-capture's cursor sessions, and this is where the pixels and
/// the geometry come from.
struct CursorCapture {
    int width = 0, height = 0;      ///< cursor image size, in buffer pixels
    int hotspot_x = 0, hotspot_y = 0;
    int x = 0, y = 0;               ///< hotspot position, in the output's pixels
    std::vector<std::uint8_t> rgba; ///< tightly packed, width*height*4
};

/// Fill `out` and return true when a cursor is visible on `output` (the
/// wl_output resource passed to `add_output`); false when it is elsewhere or
/// hidden. Without one of these registered, cursor capture sessions are still
/// created — the client's object id has to be valid either way — and told
/// immediately that they have stopped.
using ScreencopyCursorFunc = std::function<bool(wl_resource* output, CursorCapture& out)>;

/// Which window a `ext_foreign_toplevel_handle_v1` resource names, or null when
/// it names none (not one of ours, or the window has gone). The desktop module's
/// `ForeignToplevelList::toplevel_for` is exactly this.
using ToplevelResolveFunc = std::function<Toplevel*(wl_resource* handle)>;

/// A window's pixels, for a compositor that wants to draw them itself (a GPU
/// readback of a tiled dmabuf, say). Fill `rgba` with tightly-packed
/// premultiplied RGBA8, `width * height * 4` bytes, and return true. The
/// default — used when none is set — composites the window's surface tree on
/// the CPU, which reads shm, single-pixel and LINEAR dmabuf buffers.
using ToplevelCaptureFunc =
    std::function<bool(Toplevel& toplevel, int& width, int& height, std::vector<std::uint8_t>& rgba)>;

/// The wlr_screencopy_manager_v1 + ext_image_copy_capture_manager_v1 globals.
///
/// Usage:
///   1. Create the manager.
///   2. Register each output with add_output().
///   3. Clients can now capture screenshots.
///
/// Move-only; pointer-stable (pimpl) so the libwayland global is safe.
class ScreencopyManager {
public:
    [[nodiscard]] static Result<ScreencopyManager> create(Display& display);

    ~ScreencopyManager();
    ScreencopyManager(ScreencopyManager&&) noexcept;
    ScreencopyManager& operator=(ScreencopyManager&&) noexcept;
    ScreencopyManager(const ScreencopyManager&) = delete;
    ScreencopyManager& operator=(const ScreencopyManager&) = delete;

    /// Register an output for capture. `output` is the wl_resource the client
    /// passes to capture_output. `capture` is called when a client requests a copy.
    void add_output(wl_resource* output, int width, int height, ScreencopyCaptureFunc capture);

    /// Enable dmabuf capture targets. Clients may then hand a linux-dmabuf buffer
    /// instead of shm; LINEAR is written via mmap, tiled via the renderer. Without
    /// this, only shm targets are advertised.
    void set_renderer(VulkanRenderer* renderer);

    /// Where the pointer cursor comes from, for cursor capture sessions.
    void set_cursor_source(ScreencopyCursorFunc fn);

    /// Publish `ext_foreign_toplevel_image_capture_source_manager_v1`: a client
    /// that holds an ext-foreign-toplevel-list handle can capture THAT WINDOW —
    /// not the screen — through the same ext-image-copy-capture sessions an
    /// output capture uses. This is single-window screen sharing (Zoom, Teams,
    /// Discord, browsers via xdg-desktop-portal).
    ///
    /// The window is rendered from its own surface tree, so what the recipient
    /// sees does not depend on what covers it, whether it is minimised, or
    /// where it sits on the layout: capturing a window behind another one
    /// yields the window. Frames are paced by the window's own commits — a
    /// `capture` on a session whose window has not changed since the last frame
    /// is held back until it does, which is what ext-image-copy-capture asks of
    /// a compositor and what keeps an idle shared window at zero wakeups. The
    /// session is `stopped` when the window is destroyed, and re-advertises its
    /// buffer size when the window is resized.
    ///
    /// `resolve` maps a handle to a window; with the desktop module that is
    ///   `[](wl_resource* h) { return ForeignToplevelList::toplevel_for(h); }`
    /// It is a function rather than a type because `luminaria.render` does not
    /// (and, by ADR 0008, may not) depend on `luminaria.desktop`.
    [[nodiscard]] Status enable_toplevel_capture(Display& display, ToplevelResolveFunc resolve);

    /// Replace the CPU rendering of window captures (see ToplevelCaptureFunc).
    void set_toplevel_capture(ToplevelCaptureFunc fn);

    /// The cursor moved, changed shape, or left the screen. Re-reads the
    /// source and updates every live cursor session: enter/leave, position and
    /// hotspot, and fresh buffer constraints when the image changed size. Call
    /// it from wherever the compositor already moves its pointer; with no
    /// cursor sessions open it costs one empty loop.
    void notify_cursor_changed();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit ScreencopyManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// screencopy.cpp — wlr-screencopy-unstable-v1 + ext-image-copy-capture-v1 protocol glue.
//
// Implements both the legacy wlr-screencopy and the newer ext-image-copy-capture
// protocols. Only wl_shm buffers are supported (no dmabuf for now).
// RAII: each session/frame owns its wl_resource; destruction cleanly unregisters.

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
    const auto ns =
        std::chrono::duration_cast<std::chrono::duration<std::uint64_t, std::nano>>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return {
        static_cast<uint32_t>(ns / 1'000'000'000ULL >> 32),
        static_cast<uint32_t>((ns / 1'000'000'000ULL) & 0xFFFFFFFF),
        static_cast<uint32_t>(ns % 1'000'000'000ULL),
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
// Forward: cursor sessions are held by the manager, and the manager is declared
// first. Not in an anonymous namespace — a module-linkage member may not name a
// TU-local type.
struct ExtCursorSession;

struct ScreencopyManager::Impl {
    wl_display* display = nullptr;
    VulkanRenderer* renderer = nullptr; // set → dmabuf targets advertised/writable
    dev_t render_dev = 0;               // DRM render node dev_t (for ext dmabuf_device)
    bool has_dev = false;
    EventLoop loop;                     // for pacing window captures; set by enable_toplevel_capture
    ToplevelResolveFunc toplevel_resolver;
    ToplevelCaptureFunc toplevel_capture; // unset → composite the surface tree on the CPU
    CpuCompositor cpu;                    // scratch for the default window render
    ScreencopyCursorFunc cursor_source;             // unset → cursor capture unavailable
    std::vector<ExtCursorSession*> cursor_sessions; // live, weak; each unregisters itself

    // wlr-screencopy globals
    WlGlobal wlr_manager_global;

    // ext-image-copy-capture global
    WlGlobal ext_copy_capture_global;
    // ext-image-capture-source globals
    WlGlobal ext_source_output_manager_global;
    WlGlobal ext_source_toplevel_manager_global;

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

    ~Impl();
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

/// Write tightly-packed RGBA8 into the shm buffer. The shm buffer has stride
/// `shm_stride` bytes per row, format WL_SHM_FORMAT_ARGB8888.
bool write_shm_rgba(wl_resource* buffer, const std::vector<uint8_t>& rgba,
                    int width, int height, int shm_stride) {
    wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    if (!shm) { return false; }

    // This writes into a client's memory, so a bad layout here is corruption,
    // not a bad read. Re-derive the destination's own geometry instead of
    // trusting the caller's: the buffer must be at least as large as the
    // capture in both axes, and its stride must actually hold a row of
    // `width` 4-byte pixels. libwayland only checked `stride >= width`.
    const int dst_w = wl_shm_buffer_get_width(shm);
    const int dst_h = wl_shm_buffer_get_height(shm);
    const int dst_stride = wl_shm_buffer_get_stride(shm);
    if (dst_stride != shm_stride || dst_w < width || dst_h < height ||
        !layout_fits(width, height, dst_stride) ||
        rgba.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
        return false;
    }

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
    if (rgba.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
        return false;
    }
    if (dmabuf_is_linear(info.modifier)) {
        // Same reasoning as the shm path: this is a write into client memory.
        const size_t len = layout_length(width, height, info.stride, info.offset);
        if (len == 0) {
            return false;
        }
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
        // Width and height matching is not enough: libwayland validated the
        // stride as `stride >= width`, bytes against pixels, so a stride a
        // quarter of the row length is still in play and we are about to write
        // `width * 4` bytes per row into this client's pool.
        if (!layout_fits(cap_w, cap_h, shm_stride)) {
            wl_resource_post_error(f->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
                                   "buffer stride %d is too small for %d pixels per row",
                                   shm_stride, cap_w);
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
        // zwp_linux_buffer_params_v1.add validates no stride at all upstream;
        // LinuxDmabuf::build_buffer refuses a short one, but screencopy accepts
        // any wl_buffer the client hands it, so re-check at the write.
        if (!layout_fits(cap_w, cap_h, info.stride, info.offset)) {
            wl_resource_post_error(f->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
                                   "dmabuf stride %u is too small for %d pixels per row",
                                   info.stride, cap_w);
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
    .destroy = resource_destroy_request,
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

// At namespace scope, not in the anonymous namespace below: ScreencopyManager::Impl
// keeps a vector of these, and a module-linkage member may not name a TU-local
// type. Unexported, so it stays private to module luminaria all the same.
struct ExtCursorSession {
    ScreencopyManager::Impl* mgr = nullptr;
    wl_resource* resource = nullptr; // ext_image_copy_capture_cursor_session_v1
    OutputEntry* output = nullptr;
    wl_resource* inner = nullptr;    // the capture session, once asked for
    bool entered = false;
    int width = 0, height = 0;       // last advertised buffer size
    int hotspot_x = 0, hotspot_y = 0;
    int x = 0, y = 0;
};

// A cursor session is the client's and outlives us at exit (the Display,
// declared first, is what destroys a still-connected client).
ScreencopyManager::Impl::~Impl() {
    for (ExtCursorSession* cs : cursor_sessions) {
        cs->mgr = nullptr;
    }
}

// =============================================================================
// ext-image-copy-capture-v1 implementation
// =============================================================================
namespace {

// ---- ext_image_copy_capture_cursor_session_v1 ----
//
// A cursor session is the pointer as a capture source of its own, so a screen
// recorder can keep the cursor out of the video and composite it back at
// playback time. It carries the geometry (enter/leave, position, hotspot) and
// hands out an ordinary capture session for the pixels.
//
// The compositor supplies the cursor through ScreencopyManager::set_cursor_source
// and pokes notify_cursor_changed() when it moves. With no source registered
// the session object is still created — the client's new_id must be bound or
// libwayland kills the connection on its next request — and its capture
// session is told `stopped` straight away, which is the protocol's way of
// saying "this will never produce frames".
/// Ask the compositor what the pointer looks like on this session's output.
bool ext_cursor_read(ExtCursorSession& cs, CursorCapture& out) {
    if (!cs.mgr->cursor_source || cs.output == nullptr) {
        return false;
    }
    return cs.mgr->cursor_source(cs.output->output, out) && out.width > 0 && out.height > 0;
}

// ---- window capture state ----
//
// A window source is shared three ways at once — the source object, every
// session made from it, and every frame of those — and any of them can outlive
// the others, the window included. So the window lives behind shared_ptrs:
// `ToplevelTarget` follows the Toplevel (and nulls it when it dies), and
// `ToplevelSessionState` is one session's bookkeeping, which its frames share.
struct ExtFrame;
struct ToplevelSessionState;

struct ToplevelTarget : std::enable_shared_from_this<ToplevelTarget> {
    ScreencopyManager::Impl* mgr = nullptr;
    Toplevel* toplevel = nullptr; // null once the window is destroyed
    Signal<ToplevelDestroy>::Connection destroy_conn;
    // One commit connection per surface of the tree, so the frame pacing hears
    // about a video subsurface committing on its own. Rebuilt when the tree's
    // membership changes (only ever from a request handler, never mid-emit).
    std::vector<Signal<SurfaceCommit>::Connection> watch;
    std::vector<SurfaceId> watched;
    std::uint64_t serial = 0; // bumped by every commit anywhere in the tree
    bool wake_pending = false;
    std::vector<ExtFrame*> waiting;           // captures held back until `serial` moves
    std::vector<ToplevelSessionState*> sessions; // live; each unregisters itself

    void follow(Toplevel& tl);
    void refresh_watch();
    void on_commit();
    void on_destroy();
    void flush();
};

struct ToplevelSessionState {
    std::shared_ptr<ToplevelTarget> target;
    wl_resource* session = nullptr; // the ext_image_copy_capture_session_v1; null once destroyed
    int width = 0, height = 0;      // what the client was last told
    std::uint64_t last_serial = 0;  // target->serial at the last delivered frame
    bool first = true;              // nothing delivered yet: the first frame is immediate
    bool stopped = false;

    ~ToplevelSessionState() {
        if (target != nullptr) {
            std::erase(target->sessions, this);
        }
    }
};

// ---- ext_image_copy_capture_frame_v1 ----
struct ExtFrame {
    ScreencopyManager::Impl* mgr = nullptr;
    wl_resource* resource = nullptr;
    OutputEntry* output = nullptr;
    ExtCursorSession* cursor = nullptr; // set → this frame captures the pointer, not the screen
    std::shared_ptr<ToplevelSessionState> window; // set → this frame captures a window
    bool waiting = false;                         // parked in window->target->waiting
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
        if (waiting && window != nullptr && window->target != nullptr) {
            std::erase(window->target->waiting, this);
        }
    }
};

void ext_frame_forget_buffer(wl_listener* listener, void*) {
    auto* f = reinterpret_cast<ExtFrame*>(reinterpret_cast<char*>(listener) -
                                          offsetof(ExtFrame, buffer_destroy));
    f->attached_buffer = nullptr;
    wl_list_init(&f->buffer_destroy.link); // libwayland already unlinked it
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

/// Write a captured image into the buffer the client attached and answer
/// `ready` — or `failed` if what it attached cannot hold it.
void ext_frame_deliver(ExtFrame* f, const std::vector<uint8_t>& rgba, int cap_w, int cap_h) {
    // attach_buffer accepts any wl_buffer without inspecting it, and the size
    // captured here (the output's, or the cursor's) is not one the client
    // chose — so nothing so far has established that what it attached can hold
    // this frame. A 1x1 buffer against a full-screen capture would be a
    // screen-sized write into someone else's pool. Check before writing.
    if (f->attached_buffer == nullptr) {
        // Destroyed while the capture was held back.
        ext_image_copy_capture_frame_v1_send_failed(f->resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    wl_shm_buffer* shm = wl_shm_buffer_get(f->attached_buffer);
    bool wrote = false;
    if (shm) {
        const int stride = wl_shm_buffer_get_stride(shm);
        if (wl_shm_buffer_get_width(shm) >= cap_w && wl_shm_buffer_get_height(shm) >= cap_h &&
            wl_shm_buffer_get_format(shm) == WL_SHM_FORMAT_ARGB8888 &&
            layout_fits(cap_w, cap_h, stride)) {
            wrote = write_shm_rgba(f->attached_buffer, rgba, cap_w, cap_h, stride);
        }
    } else {
        // write_dmabuf_rgba() re-derives and validates the dmabuf's own layout.
        wrote = write_dmabuf_rgba(f->mgr->renderer, f->attached_buffer, rgba, cap_w, cap_h);
    }
    if (!wrote) {
        ext_image_copy_capture_frame_v1_send_failed(f->resource,
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        return;
    }

    ext_image_copy_capture_frame_v1_send_transform(f->resource, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_damage(f->resource, 0, 0, static_cast<int32_t>(cap_w),
                                                  static_cast<int32_t>(cap_h));
    auto ts = monotonic_time();
    ext_image_copy_capture_frame_v1_send_presentation_time(f->resource, ts.tv_sec_hi,
                                                             ts.tv_sec_lo, ts.tv_nsec);
    ext_image_copy_capture_frame_v1_send_ready(f->resource);
}

/// Advertise what a buffer for this session must look like: size, ARGB8888 shm,
/// plus dmabuf (device + per-format modifiers) when a renderer is enabled —
/// and `done`, which makes the batch atomic.
void ext_send_constraints(ScreencopyManager::Impl* mgr, wl_resource* session_res, int width,
                          int height) {
    ext_image_copy_capture_session_v1_send_buffer_size(session_res,
        static_cast<uint32_t>(width), static_cast<uint32_t>(height));
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

// ---- window capture ----

/// Render a window's surface tree into `rgba`. A custom function wins; the
/// default composites on the CPU with the window's geometry at the origin.
bool toplevel_render(ToplevelTarget& target, int& width, int& height, std::vector<uint8_t>& rgba) {
    Toplevel* tl = target.toplevel;
    if (tl == nullptr) {
        return false;
    }
    if (target.mgr->toplevel_capture) {
        return target.mgr->toplevel_capture(*tl, width, height, rgba) && width > 0 && height > 0;
    }
    Surface& surface = tl->surface();
    XdgGeometry g = tl->geometry();
    if (g.width <= 0 || g.height <= 0) {
        g = XdgGeometry{0, 0, surface.surface_width(), surface.surface_height()};
    }
    if (g.width <= 0 || g.height <= 0) {
        return false; // nothing committed yet
    }
    width = g.width;
    height = g.height;
    const CpuItem view = CpuView{surface.id(), -g.x, -g.y};
    target.mgr->cpu.composite(width, height, Color{0, 0, 0, 0}, std::span{&view, 1});
    const std::span<const Pixel> pixels = target.mgr->cpu.pixels();
    rgba.resize(pixels.size() * 4);
    std::memcpy(rgba.data(), pixels.data(), rgba.size());
    return true;
}

/// How big the window's capture is right now, without rendering it. 0x0 when it
/// has nothing to show.
Size toplevel_size(const ToplevelTarget& target) {
    Toplevel* tl = target.toplevel;
    if (tl == nullptr) {
        return {};
    }
    if (target.mgr->toplevel_capture) {
        return {};
    }
    XdgGeometry g = tl->geometry();
    if (g.width <= 0 || g.height <= 0) {
        g = XdgGeometry{0, 0, tl->surface().surface_width(), tl->surface().surface_height()};
    }
    return {std::max(g.width, 0), std::max(g.height, 0)};
}

void toplevel_fail(ExtFrame& f, uint32_t reason) {
    ext_image_copy_capture_frame_v1_send_failed(f.resource, reason);
}

/// Render the window into the frame's buffer. The caller has decided this frame
/// is due.
void toplevel_produce(ExtFrame& f) {
    ToplevelSessionState& st = *f.window;
    ToplevelTarget& target = *st.target;
    if (st.stopped || target.toplevel == nullptr) {
        toplevel_fail(f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }
    if (f.attached_buffer == nullptr) {
        toplevel_fail(f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!toplevel_render(target, w, h, rgba)) {
        toplevel_fail(f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    if (w != st.width || h != st.height) {
        // The window was resized since the client allocated its buffer. Say what
        // the new constraints are and fail this frame against the old ones; the
        // client reallocates and asks again.
        st.width = w;
        st.height = h;
        if (st.session != nullptr) {
            ext_send_constraints(target.mgr, st.session, w, h);
        }
        toplevel_fail(f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        return;
    }
    st.first = false;
    st.last_serial = target.serial;
    ext_frame_deliver(&f, rgba, w, h);
}

/// A capture request on a window session: deliver now if the window has changed
/// since the last frame, otherwise hold it until it does.
void toplevel_capture_frame(ExtFrame& f) {
    ToplevelSessionState& st = *f.window;
    ToplevelTarget& target = *st.target;
    if (st.stopped || target.toplevel == nullptr) {
        toplevel_fail(f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }
    target.refresh_watch();
    // A size change is news even without a commit having been heard (the tree
    // may have been reshaped by a commit that landed before we watched it).
    const Size now = toplevel_size(target);
    const bool resized = now.width > 0 && (now.width != st.width || now.height != st.height);
    if (!st.first && !resized && st.last_serial == target.serial) {
        f.waiting = true;
        target.waiting.push_back(&f);
        return;
    }
    toplevel_produce(f);
}

} // namespace

void ToplevelTarget::follow(Toplevel& tl) {
    toplevel = &tl;
    destroy_conn = tl.destroy.connect([this](ToplevelDestroy&) { on_destroy(); });
    refresh_watch();
}

void ToplevelTarget::refresh_watch() {
    if (toplevel == nullptr) {
        return;
    }
    const std::vector<SurfaceAt> tree = toplevel->surface().surface_tree();
    std::vector<SurfaceId> ids;
    ids.reserve(tree.size());
    for (const SurfaceAt& at : tree) {
        ids.push_back(at.surface->id());
    }
    if (ids == watched) {
        return;
    }
    watch.clear();
    watched = std::move(ids);
    for (const SurfaceAt& at : tree) {
        watch.push_back(at.surface->commit.connect([this](SurfaceCommit&) { on_commit(); }));
    }
}

void ToplevelTarget::on_commit() {
    ++serial;
    if (waiting.empty() || wake_pending || mgr == nullptr || !mgr->loop.valid()) {
        return;
    }
    // Not here: a commit lands in the middle of its own surface's bookkeeping
    // (an xdg geometry may still be about to change), and a video surface plus
    // its parent commit back to back. One idle turn later sees both, once.
    wake_pending = true;
    mgr->loop.once([weak = weak_from_this()] {
        if (auto self = weak.lock(); self != nullptr) {
            self->flush();
        }
    });
}

void ToplevelTarget::flush() {
    wake_pending = false;
    std::vector<ExtFrame*> frames = std::move(waiting);
    waiting.clear();
    for (ExtFrame* f : frames) {
        f->waiting = false;
    }
    for (ExtFrame* f : frames) {
        toplevel_produce(*f);
    }
}

void ToplevelTarget::on_destroy() {
    toplevel = nullptr;
    watch.clear();
    watched.clear();
    std::vector<ExtFrame*> frames = std::move(waiting);
    waiting.clear();
    for (ExtFrame* f : frames) {
        f->waiting = false;
        toplevel_fail(*f, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
    }
    const std::vector<ToplevelSessionState*> live = sessions;
    for (ToplevelSessionState* st : live) {
        if (!st->stopped) {
            st->stopped = true;
            if (st->session != nullptr) {
                ext_image_copy_capture_session_v1_send_stopped(st->session);
            }
        }
    }
}

namespace {

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

    if (f->window != nullptr) {
        toplevel_capture_frame(*f);
        return;
    }

    // Capture pixels — of the screen, or of the pointer when this frame belongs
    // to a cursor session. The cursor's size is its own, not the output's.
    std::vector<uint8_t> rgba;
    int cap_w = 0, cap_h = 0;
    if (f->cursor != nullptr) {
        CursorCapture cc;
        if (!ext_cursor_read(*f->cursor, cc)) {
            // No cursor on this output right now. Not an error the client can
            // fix by reallocating — it should wait for the next `enter`.
            ext_image_copy_capture_frame_v1_send_failed(f->resource,
                EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
            return;
        }
        cap_w = cc.width;
        cap_h = cc.height;
        rgba = std::move(cc.rgba);
    } else {
        cap_w = f->output->width;
        cap_h = f->output->height;
        if (!f->output->capture(0, 0, cap_w, cap_h, rgba)) {
            ext_image_copy_capture_frame_v1_send_failed(f->resource,
                EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
            return;
        }
    }

    ext_frame_deliver(f, rgba, cap_w, cap_h);
}

constexpr struct ext_image_copy_capture_frame_v1_interface ext_frame_impl = {
    .destroy = resource_destroy_request,
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
    ExtCursorSession* cursor = nullptr;
    std::shared_ptr<ToplevelSessionState> window; // set → a window capture session
    bool stopped = false;
};

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
    frame->cursor = s->cursor;
    frame->window = s->window;
    wl_resource_set_implementation(frame_res, &ext_frame_impl, frame, ext_frame_resource_destroy);
}

constexpr struct ext_image_copy_capture_session_v1_interface ext_session_impl = {
    .create_frame = ext_session_create_frame,
    .destroy = resource_destroy_request,
};

void ext_session_resource_destroy(wl_resource* resource) {
    auto* s = static_cast<ExtSession*>(wl_resource_get_user_data(resource));
    if (s->window != nullptr) {
        s->window->session = nullptr; // frames may outlive the session object
    }
    delete s;
}

/// What a capture source stands for. The resource's user data, so the copy
/// manager can tell an output from a window when a session is made from it.
struct ExtSource {
    OutputEntry* output = nullptr;                // set → an output source
    std::shared_ptr<ToplevelTarget> window;       // set → a window source
};

void ext_source_resource_destroy(wl_resource* resource) {
    delete static_cast<ExtSource*>(wl_resource_get_user_data(resource));
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
    auto* src = static_cast<ExtSource*>(wl_resource_get_user_data(source));
    if (src == nullptr || (src->output == nullptr && src->window == nullptr)) {
        wl_resource_post_error(mgr_resource, 0, "invalid source");
        return;
    }
    OutputEntry* out = src->output;

    wl_resource* session_res = wl_resource_create(client,
                                                    &ext_image_copy_capture_session_v1_interface,
                                                    1, id);
    if (!session_res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* session = new ExtSession{mgr, session_res, out, nullptr, nullptr, false};
    wl_resource_set_implementation(session_res, &ext_session_impl, session,
                                     ext_session_resource_destroy);

    if (src->window != nullptr) {
        // A window. If it is already gone, or has nothing on screen to size a
        // buffer by, there is nothing this session could ever produce.
        auto state = std::make_shared<ToplevelSessionState>();
        state->target = src->window;
        state->session = session_res;
        session->window = state;
        src->window->sessions.push_back(state.get());
        const Size size = toplevel_size(*src->window);
        if (src->window->toplevel == nullptr || (size.width <= 0 && !mgr->toplevel_capture)) {
            state->stopped = true;
            session->stopped = true;
            ext_image_copy_capture_session_v1_send_stopped(session_res);
            return;
        }
        // A custom renderer decides the size only when it draws: ask it once.
        int w = size.width, h = size.height;
        if (mgr->toplevel_capture) {
            std::vector<uint8_t> probe;
            if (!toplevel_render(*src->window, w, h, probe)) {
                state->stopped = true;
                session->stopped = true;
                ext_image_copy_capture_session_v1_send_stopped(session_res);
                return;
            }
        }
        state->width = w;
        state->height = h;
        src->window->refresh_watch();
        ext_send_constraints(mgr, session_res, w, h);
        return;
    }

    ext_send_constraints(mgr, session_res, out->width, out->height);
}

/// Re-read the cursor and tell the session what changed. Sends the geometry
/// events in the order the protocol requires — `enter` before any `position` or
/// `hotspot` — and re-advertises buffer constraints when the image changed size,
/// because the client's buffer is now the wrong one.
void ext_cursor_refresh(ExtCursorSession& cs) {
    CursorCapture cc;
    if (!ext_cursor_read(cs, cc)) {
        if (cs.entered) {
            cs.entered = false;
            ext_image_copy_capture_cursor_session_v1_send_leave(cs.resource);
        }
        return;
    }
    if (cs.inner != nullptr && (cc.width != cs.width || cc.height != cs.height)) {
        ext_image_copy_capture_session_v1_send_buffer_size(
            cs.inner, static_cast<uint32_t>(cc.width), static_cast<uint32_t>(cc.height));
        ext_image_copy_capture_session_v1_send_shm_format(cs.inner, WL_SHM_FORMAT_ARGB8888);
        ext_image_copy_capture_session_v1_send_done(cs.inner);
    }
    cs.width = cc.width;
    cs.height = cc.height;
    if (!cs.entered) {
        cs.entered = true;
        ext_image_copy_capture_cursor_session_v1_send_enter(cs.resource);
        // Force both below: an enter starts a fresh conversation.
        cs.x = cc.x + 1;
        cs.hotspot_x = cc.hotspot_x + 1;
    }
    if (cc.x != cs.x || cc.y != cs.y) {
        cs.x = cc.x;
        cs.y = cc.y;
        ext_image_copy_capture_cursor_session_v1_send_position(cs.resource, cc.x, cc.y);
    }
    if (cc.hotspot_x != cs.hotspot_x || cc.hotspot_y != cs.hotspot_y) {
        cs.hotspot_x = cc.hotspot_x;
        cs.hotspot_y = cc.hotspot_y;
        ext_image_copy_capture_cursor_session_v1_send_hotspot(cs.resource, cc.hotspot_x,
                                                              cc.hotspot_y);
    }
}

void ext_cursor_session_get_capture_session(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* cs = static_cast<ExtCursorSession*>(wl_resource_get_user_data(resource));
    if (cs->inner != nullptr) {
        wl_resource_post_error(resource,
                               EXT_IMAGE_COPY_CAPTURE_CURSOR_SESSION_V1_ERROR_DUPLICATE_SESSION,
                               "get_capture_session sent twice");
        return;
    }
    const uint32_t version = wl_resource_get_version(resource);
    wl_resource* session_res = wl_resource_create(
        client, &ext_image_copy_capture_session_v1_interface, static_cast<int>(version), id);
    if (!session_res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* session = new ExtSession{cs->mgr, session_res, cs->output, cs, nullptr, false};
    wl_resource_set_implementation(session_res, &ext_session_impl, session,
                                   ext_session_resource_destroy);
    cs->inner = session_res;

    CursorCapture cc;
    if (!ext_cursor_read(*cs, cc)) {
        // Either the compositor never registered a cursor source, or there is
        // no cursor to capture and no way to say "later" for a session that has
        // not advertised a size yet. Say so plainly instead of leaving the
        // client waiting on constraints that will never come.
        session->stopped = true;
        ext_image_copy_capture_session_v1_send_stopped(session_res);
        return;
    }
    cs->width = cc.width;
    cs->height = cc.height;
    ext_image_copy_capture_session_v1_send_buffer_size(
        session_res, static_cast<uint32_t>(cc.width), static_cast<uint32_t>(cc.height));
    ext_image_copy_capture_session_v1_send_shm_format(session_res, WL_SHM_FORMAT_ARGB8888);
    ext_image_copy_capture_session_v1_send_done(session_res);
    // The geometry the client needs to place what it captures.
    ext_cursor_refresh(*cs);
}

constexpr struct ext_image_copy_capture_cursor_session_v1_interface ext_cursor_session_impl = {
    .destroy = resource_destroy_request,
    .get_capture_session = ext_cursor_session_get_capture_session,
};

void ext_cursor_session_resource_destroy(wl_resource* resource) {
    auto* cs = static_cast<ExtCursorSession*>(wl_resource_get_user_data(resource));
    if (cs->mgr != nullptr) {
        std::erase(cs->mgr->cursor_sessions, cs);
    }
    delete cs;
}

void ext_manager_create_pointer_cursor_session(wl_client* client, wl_resource* mgr_resource,
                                                 uint32_t id, wl_resource* source,
                                                 wl_resource* /*pointer*/) {
    // The wl_pointer is accepted and ignored: this library has a single seat,
    // so "which pointer" and "which client" are the same question.
    auto* mgr = ext_manager_of(mgr_resource);
    auto* src = static_cast<ExtSource*>(wl_resource_get_user_data(source));
    if (src == nullptr || (src->output == nullptr && src->window == nullptr)) {
        wl_resource_post_error(mgr_resource, 0, "invalid source");
        return;
    }
    // A window has no pointer of its own to show; the session is made all the
    // same (the client's id must be bound) and, with no output, reads as "no
    // cursor here" — the same state an unregistered cursor source gives.
    OutputEntry* out = src->output;
    const uint32_t version = wl_resource_get_version(mgr_resource);
    wl_resource* resource =
        wl_resource_create(client, &ext_image_copy_capture_cursor_session_v1_interface,
                           static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* cs = new ExtCursorSession{};
    cs->mgr = mgr;
    cs->resource = resource;
    cs->output = out;
    wl_resource_set_implementation(resource, &ext_cursor_session_impl, cs,
                                   ext_cursor_session_resource_destroy);
    mgr->cursor_sessions.push_back(cs);
    // A cursor already on this output has entered as far as the client is
    // concerned; it just has nowhere to put the pixels until it asks for the
    // capture session.
    ext_cursor_refresh(*cs);
}

constexpr struct ext_image_copy_capture_manager_v1_interface ext_manager_impl = {
    .create_session = ext_manager_create_session,
    .create_pointer_cursor_session = ext_manager_create_pointer_cursor_session,
    .destroy = resource_destroy_request,
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

constexpr struct ext_image_capture_source_v1_interface ext_source_impl = {
    .destroy = resource_destroy_request,
};

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
    // The copy-capture manager resolves what the source stands for from its
    // user data later.
    wl_resource_set_implementation(source_res, &ext_source_impl, new ExtSource{out, nullptr},
                                   ext_source_resource_destroy);
}

constexpr struct ext_output_image_capture_source_manager_v1_interface ext_source_manager_impl = {
    .create_source = ext_source_manager_create_source,
    .destroy = resource_destroy_request,
};

// ---- ext_foreign_toplevel_image_capture_source_manager_v1 ----
void ext_toplevel_source_create(wl_client* client, wl_resource* mgr_resource, uint32_t id,
                                wl_resource* handle) {
    auto* mgr = ext_source_manager_of(mgr_resource);
    wl_resource* source_res =
        wl_resource_create(client, &ext_image_capture_source_v1_interface, 1, id);
    if (!source_res) {
        wl_client_post_no_memory(client);
        return;
    }
    // A handle whose window is already gone still yields a source — the id must
    // be bound — but its sessions are stopped on creation.
    auto target = std::make_shared<ToplevelTarget>();
    target->mgr = mgr;
    if (mgr != nullptr && mgr->toplevel_resolver) {
        if (Toplevel* tl = mgr->toplevel_resolver(handle); tl != nullptr) {
            target->follow(*tl);
        }
    }
    wl_resource_set_implementation(source_res, &ext_source_impl, new ExtSource{nullptr, target},
                                   ext_source_resource_destroy);
}

constexpr struct ext_foreign_toplevel_image_capture_source_manager_v1_interface
    ext_toplevel_source_manager_impl = {
        .create_source = ext_toplevel_source_create,
        .destroy = resource_destroy_request,
};

void ext_toplevel_source_manager_bind(wl_client* client, void* data, uint32_t version,
                                      uint32_t id) {
    wl_resource* resource = wl_resource_create(
        client, &ext_foreign_toplevel_image_capture_source_manager_v1_interface,
        static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &ext_toplevel_source_manager_impl, data, nullptr);
}

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
    auto global1 = create_wl_global<&zwlr_screencopy_manager_v1_interface, wlr_manager_bind>(
        display, 3, impl.get());
    if (!global1) {
        return fail(std::move(global1.error().message));
    }
    impl->wlr_manager_global = std::move(*global1);

    // ext-image-copy-capture-v1
    auto global2 = create_wl_global<&ext_image_copy_capture_manager_v1_interface, ext_manager_bind>(
        display, 1, impl.get());
    if (!global2) {
        return fail(std::move(global2.error().message));
    }
    impl->ext_copy_capture_global = std::move(*global2);

    // ext-output-image-capture-source-manager-v1
    auto global3 = create_wl_global<&ext_output_image_capture_source_manager_v1_interface, ext_source_manager_bind>(
        display, 1, impl.get());
    if (!global3) {
        return fail(std::move(global3.error().message));
    }
    impl->ext_source_output_manager_global = std::move(*global3);

    return ScreencopyManager{std::move(impl)};
}

void ScreencopyManager::add_output(wl_resource* output, int width, int height,
                                     ScreencopyCaptureFunc capture) {
    impl_->outputs.push_back(OutputEntry{output, width, height, std::move(capture)});
}

void ScreencopyManager::set_cursor_source(ScreencopyCursorFunc fn) {
    impl_->cursor_source = std::move(fn);
    notify_cursor_changed();
}

void ScreencopyManager::notify_cursor_changed() {
    // A copy: a handler cannot destroy a session from here, but the send calls
    // can fail a client and take its sessions with it.
    const std::vector<ExtCursorSession*> live = impl_->cursor_sessions;
    for (ExtCursorSession* cs : live) {
        ext_cursor_refresh(*cs);
    }
}

Status ScreencopyManager::enable_toplevel_capture(Display& display, ToplevelResolveFunc resolve) {
    impl_->toplevel_resolver = std::move(resolve);
    impl_->loop = display.event_loop();
    if (impl_->ext_source_toplevel_manager_global.get() != nullptr) {
        return ok(); // already published; only the resolver changed
    }
    auto global = create_wl_global<&ext_foreign_toplevel_image_capture_source_manager_v1_interface,
                                   ext_toplevel_source_manager_bind>(display, 1, impl_.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl_->ext_source_toplevel_manager_global = std::move(*global);
    return ok();
}

void ScreencopyManager::set_toplevel_capture(ToplevelCaptureFunc fn) {
    impl_->toplevel_capture = std::move(fn);
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
