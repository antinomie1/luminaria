// luminaria/backend/drm.cppm — bare-metal KMS backend. Drives a real monitor via
// /dev/dri/card* with **atomic** modesetting, frames paced by the DRM vblank.
//
// The scanout path is `Output::import_scanout()` + `commit_scanout()`: hand it a
// dmabuf the Vulkan renderer rendered into and it becomes a KMS framebuffer, so
// a frame goes from client buffer to screen without touching the CPU. Dumb
// buffers remain only for `commit(Color)` / `commit_frame(pixels)`.
//
// Every connected connector becomes an Output, and a udev monitor keeps that set
// live: plug a monitor in and `new_output` fires, unplug it and `Output::destroy`
// does.
//
// Requires DRM master, i.e. run from a VT with no other compositor holding the
// GPU. Pass a luminaria::Session and devices are opened through libseat, which
// is what makes VT switching safe: master is dropped on the way out and the
// modeset re-applied on the way back in.
//
// Each output drives a primary plane and, when the hardware has one to spare, a
// cursor plane — so moving the pointer costs one small atomic commit instead of
// a repaint. A client's own buffer can go straight onto the primary plane —
// see `DirectScanout` in scene/direct_scanout.cppm, which does the eligibility
// and lifetime bookkeeping on top of `import_scanout`/`release_scanout`.
// TODO: no mode switching — each output uses its connector's preferred mode.

module;

#include <memory>
#include <string>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

export module luminaria:drm;

import :backend;
import :box;
import :color;
import :dmabuf;
import :event_loop;
import :expected;
import :output;
import :pixel;
import :session;
import :signal;

export namespace luminaria {

class Session;

class DrmBackend final : public Backend {
public:
    /// Auto-detect: try each /dev/dri/card* and use the first we can master with
    /// a connected output. Fails (so callers can skip) if none works.
    ///
    /// Pass a `Session` (see luminaria/session.cppm) to open the card through
    /// libseat: the backend then drops DRM master when the VT is switched away
    /// and re-applies the modeset when it comes back. Without one the card is
    /// opened directly and VT switching corrupts the display.
    [[nodiscard]] static Result<DrmBackend> create(EventLoop loop, Session* session = nullptr);

    /// Open a specific device, become DRM master, and pick the first connected
    /// output. Fails if it can't be opened/mastered or has no output.
    [[nodiscard]] static Result<DrmBackend> create(EventLoop loop, std::string device,
                                                   Session* session = nullptr);

    ~DrmBackend();
    DrmBackend(DrmBackend&&) noexcept;
    DrmBackend& operator=(DrmBackend&&) noexcept;
    DrmBackend(const DrmBackend&) = delete;
    DrmBackend& operator=(const DrmBackend&) = delete;

    Status start() override;

    /// Re-read the connector list and diff it against the outputs we have:
    /// new monitors get an Output (and `new_output`), unplugged ones are
    /// destroyed (and `Output::destroy` fires). Called automatically on udev
    /// hotplug events once start() has run; exposed for a manual poke.
    void scan_connectors();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit DrmBackend(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Atomic modesetting only. The legacy drmModeSetCrtc/drmModePageFlip pair cannot
// express a per-frame state change across several planes and outputs at once,
// which is exactly what a cursor plane, direct scanout of a client buffer, or
// two monitors flipping on the same vblank need. Everything below builds one
// atomic request per frame; the first one carries the modeset.
namespace luminaria {

// These live at module scope rather than in an anonymous namespace on purpose:
// DrmBackend::Impl has module linkage, and a member whose signature names a
// TU-local (internal-linkage) type is ill-formed in a module interface unit.
// Nothing here is exported, so it stays private to module luminaria.

uint8_t clamp8(float c) {
    const float v = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

// --- property plumbing -------------------------------------------------------
//
// Atomic addresses everything by (object, property-id, value), and property ids
// are per-device, so each one has to be looked up by name once at start-up.

uint32_t find_prop(int fd, uint32_t obj_id, uint32_t obj_type, const char* name,
                   uint64_t* value = nullptr) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (props == nullptr) {
        return 0;
    }
    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props && id == 0; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (p == nullptr) {
            continue;
        }
        if (std::strcmp(p->name, name) == 0) {
            id = p->prop_id;
            if (value != nullptr) {
                *value = props->prop_values[i];
            }
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

struct CrtcProps {
    uint32_t mode_id = 0;
    uint32_t active = 0;
    uint32_t out_fence_ptr = 0; // optional: a fence for "this frame is on screen"
};

struct PlaneProps {
    uint32_t fb_id = 0;
    uint32_t crtc_id = 0;
    uint32_t src_x = 0, src_y = 0, src_w = 0, src_h = 0;
    uint32_t crtc_x = 0, crtc_y = 0, crtc_w = 0, crtc_h = 0;
    uint32_t in_fence_fd = 0; // optional: wait for the GPU before scanning out

    [[nodiscard]] bool complete() const noexcept {
        return fb_id && crtc_id && src_x && src_y && src_w && src_h && crtc_x && crtc_y && crtc_w &&
               crtc_h;
    }
};

// --- buffers -----------------------------------------------------------------

struct DumbFb {
    uint32_t fb_id = 0;
    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    uint8_t* map = nullptr;
};

bool create_fb(int fd, uint32_t w, uint32_t h, DumbFb& fb) {
    if (drmModeCreateDumbBuffer(fd, w, h, 32, 0, &fb.handle, &fb.pitch, &fb.size) != 0) {
        return false;
    }
    if (drmModeAddFB(fd, w, h, 24, 32, fb.pitch, fb.handle, &fb.fb_id) != 0) {
        return false;
    }
    uint64_t offset = 0;
    if (drmModeMapDumbBuffer(fd, fb.handle, &offset) != 0) {
        return false;
    }
    void* p = mmap(nullptr, fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(offset));
    if (p == MAP_FAILED) {
        return false;
    }
    fb.map = static_cast<uint8_t*>(p);
    return true;
}

void destroy_fb(int fd, DumbFb& fb) {
    if (fb.map != nullptr) {
        munmap(fb.map, fb.size);
    }
    if (fb.fb_id != 0) {
        drmModeRmFB(fd, fb.fb_id);
    }
    if (fb.handle != 0) {
        drmModeDestroyDumbBuffer(fd, fb.handle);
    }
    fb = DumbFb{};
}

// A client- or renderer-supplied dmabuf turned into a KMS framebuffer.
struct ImportedFb {
    uint32_t fb_id = 0;
    uint32_t handle = 0;
};

/// The cursor plane and its one ARGB dumb buffer. Kept separate from the primary
/// plane's state because it changes on a completely different schedule: the
/// pointer moves constantly, the screen behind it usually doesn't.
struct CursorPlane {
    uint32_t plane_id = 0;
    PlaneProps props;
    DumbFb fb;
    int width = 0, height = 0;    // what the hardware allocates (usually 64x64)
    int image_w = 0, image_h = 0; // what is actually drawn in it
    int hotspot_x = 0, hotspot_y = 0;
    int x = 0, y = 0;
    bool visible = false;
};

/// A dumb buffer for the cursor: ARGB8888 rather than the primary plane's
/// XRGB8888, because a cursor without an alpha channel is a rectangle.
bool create_cursor_fb(int fd, uint32_t w, uint32_t h, DumbFb& fb) {
    if (drmModeCreateDumbBuffer(fd, w, h, 32, 0, &fb.handle, &fb.pitch, &fb.size) != 0) {
        return false;
    }
    const uint32_t handles[4] = {fb.handle, 0, 0, 0};
    const uint32_t pitches[4] = {fb.pitch, 0, 0, 0};
    const uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb.fb_id, 0) !=
        0) {
        return false;
    }
    uint64_t offset = 0;
    if (drmModeMapDumbBuffer(fd, fb.handle, &offset) != 0) {
        return false;
    }
    void* p =
        mmap(nullptr, fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(offset));
    if (p == MAP_FAILED) {
        return false;
    }
    fb.map = static_cast<uint8_t*>(p);
    return true;
}

class DrmOutput final : public Output {
public:
    int fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t plane_id;
    uint32_t connector_crtc_id_prop;
    CrtcProps crtc_props;
    PlaneProps plane_props;
    drmModeModeInfo mode;
    uint32_t mode_blob = 0;
    bool modeset_done = false;

    drmModeCrtc* saved_crtc = nullptr; // restored when this output goes away
    DumbFb fbs[2]; // CPU path (solid colour / read-back frames)
    int front = 0;
    std::vector<ImportedFb> imported; // GPU path (renderer scanout targets, client buffers)
    std::vector<ImportedFb> retired;  // released but still on screen; freed on the next flip
    CursorPlane cursor;

    uint32_t pending_fb = 0;
    bool flip_pending = false;
    bool suspended = false; // the VT belongs to someone else right now
    bool tearing = false; // wp_tearing_control_v1: flip without waiting for vblank
    int present_fence = -1; // OUT_FENCE_PTR from the last commit, owned until taken

    /// Nominal frame duration in nanoseconds, straight from the mode timings.
    [[nodiscard]] uint32_t refresh_ns() const noexcept {
        const uint64_t pixels = static_cast<uint64_t>(mode.htotal) * mode.vtotal;
        if (mode.clock == 0 || pixels == 0) {
            return 0;
        }
        return static_cast<uint32_t>(pixels * 1000000ULL / mode.clock);
    }

    void set_tearing(bool async) override { tearing = async; }

    DrmOutput(int fd, uint32_t crtc, uint32_t connector, uint32_t plane,
              const drmModeModeInfo& mode)
        : Output(mode.hdisplay, mode.vdisplay), fd(fd), crtc_id(crtc), connector_id(connector),
          plane_id(plane), connector_crtc_id_prop(0), mode(mode) {}

    ~DrmOutput() override {
        if (saved_crtc != nullptr) {
            drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id, saved_crtc->x,
                           saved_crtc->y, nullptr, 0, nullptr);
            drmModeFreeCrtc(saved_crtc);
        }
        for (const std::vector<ImportedFb>& list : {imported, retired}) {
            for (const ImportedFb& fb : list) {
                drmModeRmFB(fd, fb.fb_id);
                drmCloseBufferHandle(fd, fb.handle);
            }
        }
        destroy_fb(fd, fbs[0]);
        destroy_fb(fd, fbs[1]);
        destroy_fb(fd, cursor.fb);
        if (mode_blob != 0) {
            drmModeDestroyPropertyBlob(fd, mode_blob);
        }
        if (present_fence >= 0) {
            close(present_fence);
        }
    }

    int take_present_fence() noexcept override {
        const int fd_out = present_fence;
        present_fence = -1;
        return fd_out;
    }

    void fill(DumbFb& fb, Color color) {
        const uint32_t pixel = (static_cast<uint32_t>(clamp8(color.r)) << 16) |
                               (static_cast<uint32_t>(clamp8(color.g)) << 8) |
                               static_cast<uint32_t>(clamp8(color.b));
        for (int y = 0; y < height_; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(fb.map + static_cast<size_t>(y) * fb.pitch);
            for (int x = 0; x < width_; ++x) {
                row[x] = pixel;
            }
        }
    }

    /// One atomic request: plane state always, plus connector/CRTC state on the
    /// modeset. `modeset` commits synchronously; every later frame is a
    /// non-blocking flip that reports back through the page-flip event.
    ///
    /// `in_fence_fd` (owned by this call) is the GPU's "the frame is drawn"
    /// fence. Handing it to KMS is the last CPU stall removed from the pipeline:
    /// we commit while the GPU is still rendering, and the display engine holds
    /// the flip until the fence signals. OUT_FENCE_PTR comes back the other way,
    /// signalling when this frame lands and the buffer it replaces is free.
    Status atomic(uint32_t fb_id, bool modeset, int in_fence_fd = -1) {
        if (suspended) {
            // Another VT owns the display; every ioctl would fail with EACCES.
            // Dropping the frame is right — nobody can see it anyway.
            if (in_fence_fd >= 0) {
                close(in_fence_fd);
            }
            return ok();
        }
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        if (req == nullptr) {
            if (in_fence_fd >= 0) {
                close(in_fence_fd);
            }
            return fail("drm: drmModeAtomicAlloc failed");
        }
        auto add = [&](uint32_t obj, uint32_t prop, uint64_t value) {
            drmModeAtomicAddProperty(req, obj, prop, value);
        };
        if (modeset) {
            add(connector_id, connector_crtc_id_prop, crtc_id);
            add(crtc_id, crtc_props.mode_id, mode_blob);
            add(crtc_id, crtc_props.active, 1);
        }
        if (in_fence_fd >= 0 && plane_props.in_fence_fd != 0) {
            add(plane_id, plane_props.in_fence_fd, static_cast<uint64_t>(in_fence_fd));
        }
        // A torn flip has no defined "on screen" moment, so don't ask for one.
        int32_t out_fence_slot = -1;
        const bool want_out_fence = crtc_props.out_fence_ptr != 0 && !(tearing && !modeset);
        if (want_out_fence) {
            add(crtc_id, crtc_props.out_fence_ptr,
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&out_fence_slot)));
        }
        add(plane_id, plane_props.fb_id, fb_id);
        add(plane_id, plane_props.crtc_id, crtc_id);
        add(plane_id, plane_props.src_x, 0);
        add(plane_id, plane_props.src_y, 0);
        add(plane_id, plane_props.src_w, static_cast<uint64_t>(width_) << 16);
        add(plane_id, plane_props.src_h, static_cast<uint64_t>(height_) << 16);
        add(plane_id, plane_props.crtc_x, 0);
        add(plane_id, plane_props.crtc_y, 0);
        add(plane_id, plane_props.crtc_w, static_cast<uint64_t>(width_));
        add(plane_id, plane_props.crtc_h, static_cast<uint64_t>(height_));

        uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
        if (modeset) {
            flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
        } else {
            flags |= DRM_MODE_ATOMIC_NONBLOCK;
            if (tearing) {
                flags |= DRM_MODE_PAGE_FLIP_ASYNC;
            }
        }
        const int rc = drmModeAtomicCommit(fd, req, flags, this);
        drmModeAtomicFree(req);
        // KMS took a reference to the fence; the fd itself stays ours to close.
        if (in_fence_fd >= 0) {
            close(in_fence_fd);
        }
        if (rc != 0) {
            if (out_fence_slot >= 0) {
                close(out_fence_slot);
            }
            return fail("drm: drmModeAtomicCommit failed");
        }
        if (out_fence_slot >= 0) {
            if (present_fence >= 0) {
                close(present_fence); // nobody took the previous one
            }
            present_fence = out_fence_slot;
        }
        pending_fb = fb_id;
        flip_pending = true;
        return ok();
    }

    Status commit(Color color) override {
        if (flip_pending) {
            return ok(); // already waiting on a flip; drop this frame
        }
        const int back = 1 - front;
        fill(fbs[back], color);
        last_committed_ = color;
        const Status s = atomic(fbs[back].fb_id, !modeset_done);
        if (s) {
            front = back;
            modeset_done = true;
        }
        return s;
    }

    Status commit_frame(std::span<const Pixel> rgba, int w, int h) override {
        if (w != width_ || h != height_ || rgba.size() != static_cast<size_t>(w) * h) {
            return fail("drm: frame size mismatch");
        }
        if (flip_pending) {
            return ok();
        }
        const int back = 1 - front;
        DumbFb& fb = fbs[back];
        for (int y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(fb.map + static_cast<size_t>(y) * fb.pitch);
            const Pixel* src = rgba.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                // Scanout is XRGB8888: 0x00RRGGBB.
                row[x] = (static_cast<uint32_t>(src[x].r) << 16) |
                         (static_cast<uint32_t>(src[x].g) << 8) | src[x].b;
            }
        }
        const Status s = atomic(fb.fb_id, !modeset_done);
        if (s) {
            front = back;
            modeset_done = true;
        }
        return s;
    }

    std::vector<std::uint64_t> scanout_modifiers(std::uint32_t drm_format) override {
        std::vector<std::uint64_t> out;
        uint64_t blob_id = 0;
        if (find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob_id) == 0) {
            return {DRM_FORMAT_MOD_LINEAR};
        }
        drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd, static_cast<uint32_t>(blob_id));
        if (blob == nullptr) {
            return {DRM_FORMAT_MOD_LINEAR};
        }
        const auto* data = static_cast<const char*>(blob->data);
        const auto* header = reinterpret_cast<const drm_format_modifier_blob*>(data);
        const auto* formats = reinterpret_cast<const uint32_t*>(data + header->formats_offset);
        const auto* mods =
            reinterpret_cast<const drm_format_modifier*>(data + header->modifiers_offset);
        for (uint32_t i = 0; i < header->count_modifiers; ++i) {
            // Each entry's `formats` bitmask indexes the format list from `offset`.
            for (uint32_t bit = 0; bit < 64; ++bit) {
                const uint32_t index = mods[i].offset + bit;
                if ((mods[i].formats & (1ULL << bit)) == 0 || index >= header->count_formats) {
                    continue;
                }
                if (formats[index] == drm_format) {
                    out.push_back(mods[i].modifier);
                    break;
                }
            }
        }
        drmModeFreePropertyBlob(blob);
        if (out.empty()) {
            out.push_back(DRM_FORMAT_MOD_LINEAR);
        }
        return out;
    }

    Result<std::uint32_t> import_scanout(const DmabufPlane& plane) override {
        if (plane.width != width_ || plane.height != height_) {
            return fail("drm: scanout buffer size does not match the mode");
        }
        uint32_t handle = 0;
        if (drmPrimeFDToHandle(fd, plane.fd, &handle) != 0) {
            return fail("drm: drmPrimeFDToHandle failed");
        }
        const uint32_t handles[4] = {handle, 0, 0, 0};
        const uint32_t pitches[4] = {plane.stride, 0, 0, 0};
        const uint32_t offsets[4] = {plane.offset, 0, 0, 0};
        const uint64_t modifiers[4] = {plane.modifier, 0, 0, 0};
        uint32_t fb_id = 0;
        const int rc = drmModeAddFB2WithModifiers(
            fd, static_cast<uint32_t>(plane.width), static_cast<uint32_t>(plane.height),
            plane.format, handles, pitches, offsets, modifiers, &fb_id,
            DRM_MODE_FB_MODIFIERS);
        if (rc != 0) {
            drmCloseBufferHandle(fd, handle);
            return fail("drm: drmModeAddFB2WithModifiers failed");
        }
        imported.push_back(ImportedFb{fb_id, handle});
        return fb_id;
    }

    /// Drop an import — the client that owned the buffer dropped it. The one
    /// the CRTC is scanning out cannot go yet (the screen would go dark on the
    /// next vblank), so it waits in `retired` until a later frame replaces it.
    void release_scanout(std::uint32_t id) override {
        const auto it = std::find_if(imported.begin(), imported.end(),
                                     [id](const ImportedFb& fb) { return fb.fb_id == id; });
        if (it == imported.end()) {
            return;
        }
        const ImportedFb fb = *it;
        imported.erase(it);
        if (fb.fb_id == pending_fb) {
            retired.push_back(fb);
            return;
        }
        drmModeRmFB(fd, fb.fb_id);
        drmCloseBufferHandle(fd, fb.handle);
    }

    /// Free everything in `retired` that is no longer the scanned-out buffer.
    void reap_retired() {
        std::erase_if(retired, [this](const ImportedFb& fb) {
            if (fb.fb_id == pending_fb) {
                return false;
            }
            drmModeRmFB(fd, fb.fb_id);
            drmCloseBufferHandle(fd, fb.handle);
            return true;
        });
    }

    // --- hardware cursor plane ---

    [[nodiscard]] bool has_cursor_plane() const noexcept override {
        return cursor.plane_id != 0 && cursor.fb.map != nullptr;
    }

    /// One atomic commit that touches only the cursor plane. Deliberately
    /// separate from the frame commit: the pointer moves far more often than
    /// the screen behind it changes, and this way it costs no repaint at all.
    Status commit_cursor() {
        if (suspended || !has_cursor_plane()) {
            return ok();
        }
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        if (req == nullptr) {
            return fail("drm: drmModeAtomicAlloc failed");
        }
        auto add = [&](uint32_t prop, uint64_t value) {
            drmModeAtomicAddProperty(req, cursor.plane_id, prop, value);
        };
        if (!cursor.visible) {
            add(cursor.props.fb_id, 0);
            add(cursor.props.crtc_id, 0);
        } else {
            // The hotspot is what tracks the pointer, so the plane's top-left
            // sits that far up and to the left of it.
            const Box logical{cursor.x - cursor.hotspot_x, cursor.y - cursor.hotspot_y,
                              cursor.width, cursor.height};
            const Box dev = to_device(logical);
            add(cursor.props.fb_id, cursor.fb.fb_id);
            add(cursor.props.crtc_id, crtc_id);
            add(cursor.props.src_x, 0);
            add(cursor.props.src_y, 0);
            add(cursor.props.src_w, static_cast<uint64_t>(cursor.width) << 16);
            add(cursor.props.src_h, static_cast<uint64_t>(cursor.height) << 16);
            add(cursor.props.crtc_x, static_cast<uint64_t>(static_cast<int64_t>(dev.x)));
            add(cursor.props.crtc_y, static_cast<uint64_t>(static_cast<int64_t>(dev.y)));
            add(cursor.props.crtc_w, static_cast<uint64_t>(dev.width));
            add(cursor.props.crtc_h, static_cast<uint64_t>(dev.height));
        }
        // No page-flip event: this commit owes us nothing, and asking for one
        // would confuse the frame pump.
        const int rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, nullptr);
        drmModeAtomicFree(req);
        return rc == 0 ? ok() : fail("drm: cursor atomic commit failed");
    }

    Status set_cursor(std::span<const std::uint8_t> rgba, int width, int height, int hotspot_x,
                      int hotspot_y) override {
        if (!has_cursor_plane()) {
            return fail("drm: this output has no cursor plane");
        }
        if (width <= 0 || height <= 0 || width > cursor.width || height > cursor.height) {
            return fail("drm: cursor image does not fit the hardware cursor size");
        }
        if (rgba.size() < static_cast<size_t>(width) * height * 4) {
            return fail("drm: cursor image is shorter than its dimensions");
        }
        cursor.image_w = width;
        cursor.image_h = height;
        cursor.hotspot_x = hotspot_x;
        cursor.hotspot_y = hotspot_y;
        // The buffer is a fixed size (64x64 as a rule) and the image may be
        // smaller, so clear first — leftovers from a bigger cursor would stay.
        std::memset(cursor.fb.map, 0, static_cast<size_t>(cursor.fb.size));
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(cursor.fb.map +
                                                    static_cast<size_t>(y) * cursor.fb.pitch);
            const std::uint8_t* src = rgba.data() + static_cast<size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                // Premultiplied RGBA in, ARGB8888 out (0xAARRGGBB).
                row[x] = (static_cast<uint32_t>(src[x * 4 + 3]) << 24) |
                         (static_cast<uint32_t>(src[x * 4 + 0]) << 16) |
                         (static_cast<uint32_t>(src[x * 4 + 1]) << 8) |
                         static_cast<uint32_t>(src[x * 4 + 2]);
            }
        }
        cursor.visible = true;
        return commit_cursor();
    }

    Status move_cursor(int x, int y) override {
        if (!has_cursor_plane()) {
            return fail("drm: this output has no cursor plane");
        }
        cursor.x = x;
        cursor.y = y;
        cursor.visible = cursor.image_w > 0;
        return commit_cursor();
    }

    Status hide_cursor() override {
        if (!has_cursor_plane()) {
            return fail("drm: this output has no cursor plane");
        }
        cursor.visible = false;
        return commit_cursor();
    }

    Status commit_scanout(std::uint32_t id, int in_fence_fd) override {
        if (std::none_of(imported.begin(), imported.end(),
                         [id](const ImportedFb& fb) { return fb.fb_id == id; })) {
            if (in_fence_fd >= 0) {
                close(in_fence_fd);
            }
            return fail("drm: unknown scanout buffer");
        }
        if (flip_pending) {
            if (in_fence_fd >= 0) {
                close(in_fence_fd);
            }
            return ok();
        }
        const Status s = atomic(id, !modeset_done, in_fence_fd);
        if (s) {
            modeset_done = true;
            reap_retired(); // whatever this frame replaced is now free to go
        }
        return s;
    }
};

void on_page_flip(int /*fd*/, unsigned seq, unsigned sec, unsigned usec, unsigned /*crtc_id*/,
                  void* data) {
    auto* out = static_cast<DrmOutput*>(data);
    out->flip_pending = false;
    // The kernel timestamps the vblank itself, on CLOCK_MONOTONIC — exactly what
    // wp_presentation wants, and better than anything we could sample here.
    PresentEvent presented{*out,
                           sec,
                           usec * 1000u,
                           out->refresh_ns(),
                           seq,
                           !out->tearing,
                           true};
    out->present.emit(presented);
    FrameEvent event{*out};
    out->frame.emit(event);
}



namespace {

/// Resolve the atomic property ids of a plane. Same set for every plane type.
PlaneProps resolve_plane_props(int fd, uint32_t plane) {
    PlaneProps pp;
    pp.fb_id = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    pp.crtc_id = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    pp.src_x = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    pp.src_y = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    pp.src_w = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    pp.src_h = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    pp.crtc_x = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    pp.crtc_y = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    pp.crtc_w = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    pp.crtc_h = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    pp.in_fence_fd = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD");
    return pp;
}

/// Fill in every atomic property id an output needs. False if the driver is
/// missing one, which means we cannot drive it atomically at all.
bool resolve_props(int fd, DrmOutput& out) {
    out.connector_crtc_id_prop =
        find_prop(fd, out.connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    out.crtc_props.mode_id = find_prop(fd, out.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    out.crtc_props.active = find_prop(fd, out.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    // Optional on old drivers; without them the pipeline still works, it just
    // has to keep the GPU and the display in step by blocking.
    out.crtc_props.out_fence_ptr =
        find_prop(fd, out.crtc_id, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR");
    out.plane_props = resolve_plane_props(fd, out.plane_id);
    const PlaneProps& pp = out.plane_props;
    return out.connector_crtc_id_prop != 0 && out.crtc_props.mode_id != 0 &&
           out.crtc_props.active != 0 && pp.complete();
}

/// A CRTC this connector can drive that nothing else has taken, as (id, index).
/// The index matters: plane routing is a bitmask over CRTC indices.
bool pick_crtc(int fd, drmModeRes* res, drmModeConnector* connector,
               const std::vector<uint32_t>& taken, uint32_t& crtc_id, int& crtc_index) {
    auto free_crtc = [&](int i) {
        return std::find(taken.begin(), taken.end(), res->crtcs[i]) == taken.end();
    };
    // Prefer the CRTC the connector is already lit by: no flicker, no reshuffle.
    if (drmModeEncoder* enc = drmModeGetEncoder(fd, connector->encoder_id); enc != nullptr) {
        for (int i = 0; i < res->count_crtcs; ++i) {
            if (res->crtcs[i] == enc->crtc_id && free_crtc(i)) {
                crtc_id = res->crtcs[i];
                crtc_index = i;
                drmModeFreeEncoder(enc);
                return true;
            }
        }
        drmModeFreeEncoder(enc);
    }
    for (int e = 0; e < connector->count_encoders; ++e) {
        drmModeEncoder* enc = drmModeGetEncoder(fd, connector->encoders[e]);
        if (enc == nullptr) {
            continue;
        }
        for (int i = 0; i < res->count_crtcs; ++i) {
            if ((enc->possible_crtcs & (1u << i)) != 0 && free_crtc(i)) {
                crtc_id = res->crtcs[i];
                crtc_index = i;
                drmModeFreeEncoder(enc);
                return true;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return false;
}

/// A plane of `want_type` for `crtc_index` that no other output claimed.
uint32_t pick_plane(int fd, int crtc_index, uint64_t want_type,
                    const std::vector<uint32_t>& taken) {
    drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
    if (planes == nullptr) {
        return 0;
    }
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < planes->count_planes && chosen == 0; ++i) {
        drmModePlane* plane = drmModeGetPlane(fd, planes->planes[i]);
        if (plane == nullptr) {
            continue;
        }
        uint64_t type = 0;
        if ((plane->possible_crtcs & (1u << crtc_index)) != 0 &&
            std::find(taken.begin(), taken.end(), plane->plane_id) == taken.end() &&
            find_prop(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type) != 0 &&
            type == want_type) {
            chosen = plane->plane_id;
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planes);
    return chosen;
}

/// Claim a cursor plane for this output, if the hardware has a spare one.
/// Optional by design — everything still works without it, the compositor just
/// has to draw the pointer into the frame.
void setup_cursor_plane(int fd, DrmOutput& out, int crtc_index,
                        const std::vector<uint32_t>& taken) {
    const uint32_t plane = pick_plane(fd, crtc_index, DRM_PLANE_TYPE_CURSOR, taken);
    if (plane == 0) {
        return;
    }
    uint64_t width = 64, height = 64;
    drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &width);
    drmGetCap(fd, DRM_CAP_CURSOR_HEIGHT, &height);
    CursorPlane& cursor = out.cursor;
    cursor.plane_id = plane;
    cursor.props = resolve_plane_props(fd, plane);
    cursor.width = static_cast<int>(width);
    cursor.height = static_cast<int>(height);
    if (!cursor.props.complete() ||
        !create_cursor_fb(fd, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                          cursor.fb)) {
        destroy_fb(fd, cursor.fb);
        cursor.plane_id = 0;
    }
}

} // namespace

struct DrmBackend::Impl {
    EventLoop loop;
    int fd = -1;
    std::vector<std::unique_ptr<DrmOutput>> outputs;
    EventSource drm_source;

    // Hotplug: udev tells us a connector changed, we re-scan and diff.
    udev* udev_ctx = nullptr;
    udev_monitor* monitor = nullptr;
    EventSource udev_source;
    bool started = false;

    // Session (libseat), when the caller gave us one. It owns the card fd and
    // tells us when the VT is taken away.
    Session* session = nullptr;
    int device_id = -1;
    Signal<SessionActive>::Connection session_conn;

    ~Impl() {
        outputs.clear(); // each output restores its own CRTC
        if (monitor != nullptr) {
            udev_monitor_unref(monitor);
        }
        if (udev_ctx != nullptr) {
            udev_unref(udev_ctx);
        }
        if (session != nullptr && device_id >= 0) {
            session->close_device(device_id); // the session owns the fd
            return;
        }
        if (fd >= 0) {
            drmDropMaster(fd);
            close(fd);
        }
    }

    [[nodiscard]] std::vector<uint32_t> taken_crtcs() const {
        std::vector<uint32_t> ids;
        for (const auto& out : outputs) {
            ids.push_back(out->crtc_id);
        }
        return ids;
    }
    [[nodiscard]] std::vector<uint32_t> taken_planes() const {
        std::vector<uint32_t> ids;
        for (const auto& out : outputs) {
            ids.push_back(out->plane_id);
        }
        return ids;
    }
    [[nodiscard]] bool has_connector(uint32_t connector_id) const {
        return std::any_of(outputs.begin(), outputs.end(), [connector_id](const auto& out) {
            return out->connector_id == connector_id;
        });
    }

    /// Build an output for a connected connector, or null if the GPU has no
    /// spare CRTC/plane for it (more monitors than pipes).
    std::unique_ptr<DrmOutput> make_output(drmModeRes* res, drmModeConnector* connector) {
        uint32_t crtc_id = 0;
        int crtc_index = -1;
        if (!pick_crtc(fd, res, connector, taken_crtcs(), crtc_id, crtc_index)) {
            return nullptr;
        }
        const uint32_t plane_id =
            pick_plane(fd, crtc_index, DRM_PLANE_TYPE_PRIMARY, taken_planes());
        if (plane_id == 0) {
            return nullptr;
        }
        const drmModeModeInfo mode = connector->modes[0]; // preferred mode first
        auto out = std::make_unique<DrmOutput>(fd, crtc_id, connector->connector_id, plane_id, mode);
        if (!resolve_props(fd, *out)) {
            return nullptr;
        }
        if (drmModeCreatePropertyBlob(fd, &out->mode, sizeof(out->mode), &out->mode_blob) != 0 ||
            !create_fb(fd, mode.hdisplay, mode.vdisplay, out->fbs[0]) ||
            !create_fb(fd, mode.hdisplay, mode.vdisplay, out->fbs[1])) {
            return nullptr;
        }
        // Optional: a cursor plane means the pointer moves without repainting.
        std::vector<uint32_t> claimed = taken_planes();
        claimed.push_back(plane_id);
        setup_cursor_plane(fd, *out, crtc_index, claimed);
        out->saved_crtc = drmModeGetCrtc(fd, crtc_id);
        return out;
    }

    /// Light an output up: black frame, modeset, and the page-flip chain starts.
    Status light_up(DrmOutput& out) {
        out.fill(out.fbs[0], Color{0, 0, 0, 1});
        if (Status s = out.atomic(out.fbs[0].fb_id, true); !s) {
            return s;
        }
        out.front = 0;
        out.modeset_done = true;
        return ok();
    }
};

DrmBackend::DrmBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DrmBackend::~DrmBackend() = default;
DrmBackend::DrmBackend(DrmBackend&&) noexcept = default;
DrmBackend& DrmBackend::operator=(DrmBackend&&) noexcept = default;

Result<DrmBackend> DrmBackend::create(EventLoop loop, Session* session) {
    Result<DrmBackend> last = fail("drm: no /dev/dri/card* found");
    for (int i = 0; i < 16; ++i) {
        std::string device = "/dev/dri/card" + std::to_string(i);
        if (access(device.c_str(), F_OK) != 0) {
            continue;
        }
        auto result = create(loop, device, session);
        if (result) {
            return result;
        }
        last = std::move(result); // remember why the last candidate failed
    }
    return last;
}

Result<DrmBackend> DrmBackend::create(EventLoop loop, std::string device, Session* session) {
    auto impl = std::make_unique<Impl>();
    impl->loop = loop;
    impl->session = session;

    int fd = -1;
    if (session != nullptr) {
        // Through the seat: logind hands us the fd and can revoke it, which is
        // what makes VT switching safe.
        auto opened = session->open_device(device.c_str(), impl->device_id);
        if (!opened) {
            return fail(opened.error().message);
        }
        fd = *opened;
    } else {
        fd = open(device.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return fail("drm: cannot open " + device);
        }
    }
    impl->fd = fd; // Impl's destructor owns the fd from here on
    if (drmSetMaster(fd) != 0 && session == nullptr) {
        // With a session, logind grants master on activation; without one this
        // is the only chance and failing means another compositor holds it.
        return fail("drm: not DRM master (run from a free VT with no compositor)");
    }

    // Universal planes must come first: without it the primary plane is hidden,
    // and atomic needs to address it directly.
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        return fail("drm: driver has no atomic modesetting support");
    }

    auto backend = DrmBackend{std::move(impl)};
    backend.scan_connectors();
    if (backend.impl_->outputs.empty()) {
        return fail("drm: no usable connected output");
    }
    return backend;
}

void DrmBackend::scan_connectors() {
    Impl& impl = *impl_;
    drmModeRes* res = drmModeGetResources(impl.fd);
    if (res == nullptr) {
        return;
    }

    std::vector<uint32_t> connected;
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector* connector = drmModeGetConnector(impl.fd, res->connectors[i]);
        if (connector == nullptr) {
            continue;
        }
        const bool usable =
            connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0;
        if (usable) {
            connected.push_back(connector->connector_id);
        }
        if (usable && !impl.has_connector(connector->connector_id)) {
            if (std::unique_ptr<DrmOutput> out = impl.make_output(res, connector)) {
                DrmOutput& added = *out;
                impl.outputs.push_back(std::move(out));
                // Before start(), the initial set is announced and lit there —
                // announcing here too would hand the compositor each output twice.
                if (impl.started) {
                    NewOutput event{added};
                    new_output.emit(event);
                    (void)impl.light_up(added);
                }
            }
        }
        drmModeFreeConnector(connector);
    }
    drmModeFreeResources(res);

    // Anything no longer connected goes away; ~DrmOutput emits Output::destroy,
    // which is how the compositor learns to drop its per-output state.
    std::erase_if(impl.outputs, [&connected](const std::unique_ptr<DrmOutput>& out) {
        return std::find(connected.begin(), connected.end(), out->connector_id) == connected.end();
    });
}

Status DrmBackend::start() {
    // VT switching. Give up DRM master when the session goes away, take it back
    // and re-apply the modeset when it returns — the other session will have
    // left the CRTCs pointing somewhere else entirely.
    if (impl_->session != nullptr) {
        impl_->session_conn =
            impl_->session->activity().connect([this](SessionActive& event) {
                for (auto& out : impl_->outputs) {
                    out->suspended = !event.active;
                }
                if (!event.active) {
                    drmDropMaster(impl_->fd);
                    return;
                }
                drmSetMaster(impl_->fd);
                for (auto& out : impl_->outputs) {
                    // The flip we were waiting for died with the session, and
                    // the mode is no longer ours: start the pump over.
                    out->flip_pending = false;
                    out->modeset_done = false;
                    (void)impl_->light_up(*out);
                }
            });
    }

    // Route DRM events (page-flip completions) through our loop first: the
    // modesets below already ask for one each.
    impl_->drm_source = impl_->loop.add_fd(impl_->fd, [this] {
        drmEventContext ctx{};
        ctx.version = 3;
        ctx.page_flip_handler2 = on_page_flip;
        drmHandleEvent(impl_->fd, &ctx);
    });

    // Monitor hotplug. A monitor plugged in after start-up is the normal case on
    // a laptop with a dock, and without this it simply never appears.
    impl_->udev_ctx = udev_new();
    if (impl_->udev_ctx != nullptr) {
        impl_->monitor = udev_monitor_new_from_netlink(impl_->udev_ctx, "udev");
    }
    if (impl_->monitor != nullptr &&
        udev_monitor_filter_add_match_subsystem_devtype(impl_->monitor, "drm", nullptr) == 0 &&
        udev_monitor_enable_receiving(impl_->monitor) == 0) {
        impl_->udev_source = impl_->loop.add_fd(udev_monitor_get_fd(impl_->monitor), [this] {
            udev_device* dev = udev_monitor_receive_device(impl_->monitor);
            if (dev == nullptr) {
                return;
            }
            const char* hotplug = udev_device_get_property_value(dev, "HOTPLUG");
            udev_device_unref(dev);
            // Re-scan on any DRM hotplug: the event says "something changed",
            // not what, and a full re-scan is cheap at monitor-plug rates.
            if (hotplug != nullptr && std::strcmp(hotplug, "1") == 0) {
                scan_connectors();
            }
        });
    }

    for (auto& out : impl_->outputs) {
        NewOutput event{*out};
        new_output.emit(event);
    }
    // The modeset doubles as each output's first frame: black on screen, and its
    // completion event drives the compositor's first real commit.
    for (auto& out : impl_->outputs) {
        if (Status s = impl_->light_up(*out); !s) {
            return s;
        }
    }
    impl_->started = true;
    return ok();
}

} // namespace luminaria
