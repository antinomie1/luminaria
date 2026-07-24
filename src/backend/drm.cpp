#include "luminaria/backend/drm.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace luminaria {

namespace {

uint8_t clamp8(float c) {
    const float v = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

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

class DrmOutput final : public Output {
public:
    int fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    drmModeModeInfo mode;
    DumbFb fbs[2];
    int front = 0;
    int pending = -1;
    bool flip_pending = false;

    DrmOutput(int fd, uint32_t crtc, uint32_t connector, const drmModeModeInfo& mode)
        : Output(mode.hdisplay, mode.vdisplay), fd(fd), crtc_id(crtc), connector_id(connector),
          mode(mode) {}

    ~DrmOutput() override {
        destroy_fb(fd, fbs[0]);
        destroy_fb(fd, fbs[1]);
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

    Status flip(int back) {
        if (drmModePageFlip(fd, crtc_id, fbs[back].fb_id, DRM_MODE_PAGE_FLIP_EVENT, this) != 0) {
            return fail("drmModePageFlip failed");
        }
        pending = back;
        flip_pending = true;
        return ok();
    }

    Status commit(Color color) override {
        if (flip_pending) {
            return ok(); // already waiting on a flip; drop this frame
        }
        const int back = 1 - front;
        fill(fbs[back], color);
        return flip(back);
    }

    Status commit_frame(std::span<const Pixel> rgba, int w, int h) override {
        if (w != width_ || h != height_ ||
            rgba.size() != static_cast<size_t>(w) * h) {
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
        return flip(back);
    }
};

void on_page_flip(int /*fd*/, unsigned /*seq*/, unsigned /*sec*/, unsigned /*usec*/, void* data) {
    auto* out = static_cast<DrmOutput*>(data);
    out->front = out->pending;
    out->flip_pending = false;
    FrameEvent event{*out};
    out->frame.emit(event);
}

} // namespace

struct DrmBackend::Impl {
    EventLoop loop;
    int fd = -1;
    drmModeCrtc* saved_crtc = nullptr;
    std::unique_ptr<DrmOutput> output;
    EventSource drm_source;

    ~Impl() {
        output.reset();
        if (fd >= 0) {
            if (saved_crtc != nullptr) {
                drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id, saved_crtc->x,
                               saved_crtc->y, nullptr, 0, nullptr);
                drmModeFreeCrtc(saved_crtc);
            }
            drmDropMaster(fd);
            close(fd);
        }
    }
};

DrmBackend::DrmBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DrmBackend::~DrmBackend() = default;
DrmBackend::DrmBackend(DrmBackend&&) noexcept = default;
DrmBackend& DrmBackend::operator=(DrmBackend&&) noexcept = default;

Result<DrmBackend> DrmBackend::create(EventLoop loop) {
    Result<DrmBackend> last = fail("drm: no /dev/dri/card* found");
    for (int i = 0; i < 16; ++i) {
        std::string device = "/dev/dri/card" + std::to_string(i);
        if (access(device.c_str(), F_OK) != 0) {
            continue;
        }
        auto result = create(loop, device);
        if (result) {
            return result;
        }
        last = std::move(result); // remember why the last candidate failed
    }
    return last;
}

Result<DrmBackend> DrmBackend::create(EventLoop loop, std::string device) {
    int fd = open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return fail("drm: cannot open " + device);
    }
    if (drmSetMaster(fd) != 0) {
        close(fd);
        return fail("drm: not DRM master (run from a free VT with no compositor)");
    }

    auto impl = std::make_unique<Impl>();
    impl->loop = loop;
    impl->fd = fd;

    drmModeRes* res = drmModeGetResources(fd);
    if (res == nullptr) {
        return fail("drm: drmModeGetResources failed");
    }

    drmModeConnector* connector = nullptr;
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (c != nullptr && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            connector = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    if (connector == nullptr) {
        drmModeFreeResources(res);
        return fail("drm: no connected output");
    }

    // Resolve a CRTC via the connector's encoder.
    uint32_t crtc_id = 0;
    drmModeEncoder* enc = drmModeGetEncoder(fd, connector->encoder_id);
    if (enc != nullptr) {
        crtc_id = enc->crtc_id;
        drmModeFreeEncoder(enc);
    }
    if (crtc_id == 0 && res->count_crtcs > 0) {
        crtc_id = res->crtcs[0];
    }
    const drmModeModeInfo mode = connector->modes[0];
    const uint32_t connector_id = connector->connector_id;
    drmModeFreeConnector(connector);
    drmModeFreeResources(res);
    if (crtc_id == 0) {
        return fail("drm: no CRTC");
    }

    impl->saved_crtc = drmModeGetCrtc(fd, crtc_id);
    impl->output = std::make_unique<DrmOutput>(fd, crtc_id, connector_id, mode);
    if (!create_fb(fd, mode.hdisplay, mode.vdisplay, impl->output->fbs[0]) ||
        !create_fb(fd, mode.hdisplay, mode.vdisplay, impl->output->fbs[1])) {
        return fail("drm: dumb buffer allocation failed");
    }

    return DrmBackend{std::move(impl)};
}

Status DrmBackend::start() {
    DrmOutput* out = impl_->output.get();
    out->fill(out->fbs[0], Color{0, 0, 0, 1});

    // Modeset: show the first framebuffer.
    if (drmModeSetCrtc(impl_->fd, out->crtc_id, out->fbs[0].fb_id, 0, 0, &out->connector_id, 1,
                       &out->mode) != 0) {
        return fail("drm: drmModeSetCrtc failed");
    }
    out->front = 0;

    // Route DRM events (page-flip completions) through our loop.
    impl_->drm_source = impl_->loop.add_fd(impl_->fd, [this] {
        drmEventContext ctx{};
        ctx.version = 2;
        ctx.page_flip_handler = on_page_flip;
        drmHandleEvent(impl_->fd, &ctx);
    });

    NewOutput event{*out};
    new_output.emit(event);

    // Kick the first frame so the compositor commits and the flip chain begins.
    impl_->loop.once([out] {
        FrameEvent frame{*out};
        out->frame.emit(frame);
    });
    return ok();
}

} // namespace luminaria
