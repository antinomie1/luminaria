// luminaria/shell/direct_scanout.cppm — put a client's buffer on the screen
// without drawing anything.
//
// The usual frame is: import each client buffer as a texture, composite them
// into a render target, flip to the target. When exactly one window covers the
// whole output, all of that is a copy of a buffer the client already produced in
// the layout the display wants — a fullscreen video player or game is doing
// nothing but handing us finished frames. Direct scanout skips the compositor
// entirely and points the CRTC at the client's buffer.
//
// What makes it hard is not the flip, it is the bookkeeping. A client rotates
// two or three buffers, so importing on every frame would cost a
// drmModeAddFB2 per frame and leak framebuffers; and the buffer belongs to the
// client, which can destroy it whenever it likes. So this keeps a cache keyed on
// the wl_buffer with a destroy listener on each entry — the same shape as
// `Surface::BufferWatch`, and for the same reason.
//
//     DirectScanout direct{output};
//     if (auto id = direct.id_for(*fullscreen_surface)) {
//         output.commit_scanout(*id);   // nothing was rendered at all
//     } else {
//         ... composite as usual ...
//     }
//
// `id_for` returning nullopt is the normal case, not an error: the surface is
// the wrong size, the client used shm, the buffer is in a layout the display
// cannot read. The caller composites instead.

module;

#include "detail/wayland_fwd.h"
#include <cstdint>
#include <memory>
#include <optional>

#include <algorithm>
#include <cstddef>
#include <vector>
#include <wayland-server-core.h>

export module luminaria:direct_scanout;

import :compositor;
import :dmabuf;
import :output;
import :signal;
import :transform;

export namespace luminaria {

/// Per-output cache of client buffers imported as scanout framebuffers.
/// Move-only, and tied to the `Output` it was built for — drop it in the
/// output's `destroy` handler.
class DirectScanout {
public:
    explicit DirectScanout(Output& output);

    ~DirectScanout();
    DirectScanout(DirectScanout&&) noexcept;
    DirectScanout& operator=(DirectScanout&&) noexcept;
    DirectScanout(const DirectScanout&) = delete;
    DirectScanout& operator=(const DirectScanout&) = delete;

    /// True if this output can take a client buffer at all. False for headless
    /// and for a nested window whose parent has no linux-dmabuf, in which case
    /// `id_for` always says no and the caller can skip asking.
    [[nodiscard]] bool available() const noexcept;

    /// The scanout id for `surface`'s current buffer, ready to hand to
    /// `Output::commit_scanout()`, or nullopt when this surface cannot go
    /// straight to the screen. Cheap to call every frame: an unchanged buffer
    /// is answered out of the cache, and a buffer that was refused once is
    /// remembered as refused.
    ///
    /// Says no unless the buffer is *exactly* what the display would scan out
    /// anyway: a dmabuf, the size of the mode, unrotated, uncropped, unscaled,
    /// and in a layout the output advertised. Anything else and the pixels on
    /// screen would not be the pixels the client asked for.
    [[nodiscard]] std::optional<std::uint32_t> id_for(const Surface& surface);

    /// "That id is now committed to the output." Holds the surface's buffer so
    /// the client is not told it may redraw into a frame the display hardware
    /// is still reading, and remembers the one it replaced.
    ///
    /// Call it right after a successful `commit_scanout`, and call `presented()`
    /// from the output's `present` handler — between the two, TWO client
    /// buffers are on the hook: the one that was just queued and the one still
    /// lit up until the flip lands.
    void committed(Surface& surface);

    /// The queued flip reached the screen. Whatever it replaced is off the
    /// display now, so its release can finally go out. Call from `present`.
    void presented();

    /// Release every import. Call after a mode change — the framebuffers were
    /// sized for the old mode and none of them can be scanned out now.
    void clear();

    /// Cached imports, live and refused. Diagnostics; a well-behaved client
    /// settles at the size of its swapchain.
    [[nodiscard]] std::size_t cached() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation

namespace luminaria {

// Not in an anonymous namespace: DirectScanout::Impl holds these, and a
// module-linkage class may not have a member naming an internal-linkage type.
struct DsEntry;

/// A buffer we told the client not to touch, and the surface that owns it. The
/// surface can be destroyed while its buffer is on screen, so the pointer is
/// cleared from `destroy` rather than trusted.
struct DsHold {
    Surface* surface = nullptr;
    wl_resource* buffer = nullptr;
    Signal<SurfaceDestroy>::Connection gone;

    DsHold() = default;
    // Neither copyable nor movable: `gone`'s handler captures this address, so
    // a moved DsHold would clear the fields of the object it came from.
    DsHold(const DsHold&) = delete;
    DsHold& operator=(const DsHold&) = delete;

    void release() {
        if (surface != nullptr && buffer != nullptr) {
            surface->unhold_buffer(buffer);
        }
        surface = nullptr;
        buffer = nullptr;
        gone = {};
    }

    void take(Surface& s, wl_resource* b) {
        surface = &s;
        buffer = b;
        watch();
        s.hold_buffer(b);
    }

    /// Move the hold out of `other` without releasing it — the buffer stays
    /// held, it just changes which slot is responsible for it.
    void adopt(DsHold& other) {
        surface = other.surface;
        buffer = other.buffer;
        watch();
        other.surface = nullptr;
        other.buffer = nullptr;
        other.gone = {};
    }

private:
    void watch() {
        gone = surface == nullptr ? Signal<SurfaceDestroy>::Connection{}
                                  : surface->destroy.connect([this](SurfaceDestroy&) {
                                        // The surface is going and its buffers
                                        // with it; unholding through a dying
                                        // object would be a use-after-free.
                                        surface = nullptr;
                                        buffer = nullptr;
                                    });
    }
};

struct DirectScanout::Impl {
    Output* output = nullptr;
    // The monitor can be unplugged with buffers still imported. It frees them
    // itself on the way out; all we must do is stop calling into it.
    Signal<OutputDestroy>::Connection output_gone;
    // Entries are held by pointer: each one carries a wl_listener whose address
    // libwayland keeps, so it must not move when the vector grows.
    std::vector<std::unique_ptr<DsEntry>> entries;
    DsHold on_screen; // committed most recently — lit, or about to be
    DsHold previous;  // what the pending flip is replacing; still lit until it lands

    ~Impl();
    [[nodiscard]] DsEntry* find(wl_resource* buffer) const;
    void forget(DsEntry* entry);
};

using DsImpl = DirectScanout::Impl;

struct DsEntry {
    wl_listener destroy{};   // on the client's wl_buffer; first, for the offsetof
    DsImpl* owner = nullptr;
    wl_resource* buffer = nullptr;
    std::uint32_t id = 0;
    bool usable = false;     // false = this buffer was tried and refused
};

namespace {

void ds_buffer_destroy(wl_listener* listener, void*) {
    // The client dropped the buffer. Its import has to go with it — the
    // framebuffer would otherwise pin pages the client thinks it freed.
    auto* entry = reinterpret_cast<DsEntry*>(listener);
    entry->owner->forget(entry);
}

/// Is this surface presenting its buffer 1:1, with nothing in between? A crop,
/// a stretch, a rotation or a buffer scale all mean the screen is supposed to
/// show something other than the buffer's own pixels.
bool presented_verbatim(const Surface& surface) {
    if (surface.buffer_transform() != Transform::normal) {
        return false;
    }
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    surface.buffer_source_uv(u0, v0, u1, v1);
    return u0 == 0.0f && v0 == 0.0f && u1 == 1.0f && v1 == 1.0f;
}

} // namespace

DsEntry* DsImpl::find(wl_resource* buffer) const {
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [buffer](const std::unique_ptr<DsEntry>& e) {
                                     return e->buffer == buffer;
                                 });
    return it == entries.end() ? nullptr : it->get();
}

void DsImpl::forget(DsEntry* entry) {
    if (entry->usable && output != nullptr) {
        output->release_scanout(entry->id);
    }
    wl_list_remove(&entry->destroy.link);
    std::erase_if(entries, [entry](const std::unique_ptr<DsEntry>& e) { return e.get() == entry; });
}

DsImpl::~Impl() {
    previous.release();
    on_screen.release();
    while (!entries.empty()) {
        forget(entries.back().get());
    }
}

DirectScanout::DirectScanout(Output& output) : impl_(std::make_unique<Impl>()) {
    impl_->output = &output;
    impl_->output_gone = output.destroy.connect([impl = impl_.get()](OutputDestroy&) {
        impl->output = nullptr;
    });
}
DirectScanout::~DirectScanout() = default;
DirectScanout::DirectScanout(DirectScanout&&) noexcept = default;
DirectScanout& DirectScanout::operator=(DirectScanout&&) noexcept = default;

bool DirectScanout::available() const noexcept {
    // A format-agnostic question, so ask about the one format any scanout
    // engine handles; an output with no dmabuf path at all reports nothing for
    // it either.
    return impl_->output != nullptr &&
           !impl_->output->scanout_modifiers(0x34325258 /* DRM_FORMAT_XRGB8888 */).empty();
}

std::size_t DirectScanout::cached() const noexcept { return impl_->entries.size(); }

void DirectScanout::clear() {
    impl_->previous.release();
    impl_->on_screen.release();
    while (!impl_->entries.empty()) {
        impl_->forget(impl_->entries.back().get());
    }
}

void DirectScanout::committed(Surface& surface) {
    // Anything still in `previous` is two flips old by now; nothing can be
    // reading it.
    impl_->previous.release();
    impl_->previous.adopt(impl_->on_screen);
    impl_->on_screen.take(surface, surface.current_buffer());
}

void DirectScanout::presented() { impl_->previous.release(); }

std::optional<std::uint32_t> DirectScanout::id_for(const Surface& surface) {
    wl_resource* buffer = surface.current_buffer();
    if (impl_->output == nullptr || buffer == nullptr) {
        return std::nullopt;
    }
    Output& output = *impl_->output;
    // Answered from the cache, including the "no" answers: a client that hands
    // us an unscannable buffer every frame must not cost a failed import
    // every frame.
    if (DsEntry* cached = impl_->find(buffer); cached != nullptr) {
        return cached->usable ? std::optional<std::uint32_t>{cached->id} : std::nullopt;
    }

    auto entry = std::make_unique<DsEntry>();
    entry->owner = impl_.get();
    entry->buffer = buffer;
    entry->destroy.notify = ds_buffer_destroy;
    wl_resource_add_destroy_listener(buffer, &entry->destroy);
    DsEntry* raw = entry.get();
    impl_->entries.push_back(std::move(entry));

    DmabufPlane plane{};
    if (!presented_verbatim(surface) || !surface.current_buffer_dmabuf(plane) ||
        plane.width != output.width() || plane.height != output.height()) {
        return std::nullopt; // cached as unusable
    }
    const std::vector<std::uint64_t> mods = output.scanout_modifiers(plane.format);
    if (std::find(mods.begin(), mods.end(), plane.modifier) == mods.end()) {
        return std::nullopt;
    }
    auto id = output.import_scanout(plane);
    if (!id) {
        return std::nullopt;
    }
    raw->id = *id;
    raw->usable = true;
    return *id;
}

} // namespace luminaria
