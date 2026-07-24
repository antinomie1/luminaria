#include "luminaria/compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <ctime>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "luminaria/core/display.hpp"
#include "luminaria/linux_dmabuf.hpp"

namespace luminaria {

struct Compositor::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Signal<NewSurface> new_surface;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

bool Surface::current_buffer_rgba(std::vector<std::uint8_t>& out, int& width, int& height) const {
    if (current_buffer_ == nullptr) {
        return false;
    }
    wl_shm_buffer* shm = wl_shm_buffer_get(current_buffer_);
    if (shm == nullptr) {
        // Not shm: try a linux-dmabuf buffer (LINEAR, mmap'd CPU-side).
        return dmabuf_buffer_to_rgba(current_buffer_, out, width, height);
    }
    const uint32_t format = wl_shm_buffer_get_format(shm);
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        return false;
    }
    const int w = wl_shm_buffer_get_width(shm);
    const int h = wl_shm_buffer_get_height(shm);
    const int stride = wl_shm_buffer_get_stride(shm);
    const bool opaque = format == WL_SHM_FORMAT_XRGB8888;

    wl_shm_buffer_begin_access(shm);
    const auto* data = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));
    out.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = data + static_cast<size_t>(y) * stride;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            // shm ARGB8888 is little-endian: bytes are B,G,R,A. Emit RGBA.
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = opaque ? 255 : src[x * 4 + 3];
        }
    }
    wl_shm_buffer_end_access(shm);
    width = w;
    height = h;
    return true;
}

namespace {
uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Buffer extent without decoding pixels: shm knows it directly, dmabuf carries
// it in the plane metadata. Used for hit-testing and subsurface layout.
void buffer_size(wl_resource* buffer, int& w, int& h) {
    w = 0;
    h = 0;
    if (buffer == nullptr) {
        return;
    }
    if (wl_shm_buffer* shm = wl_shm_buffer_get(buffer); shm != nullptr) {
        w = wl_shm_buffer_get_width(shm);
        h = wl_shm_buffer_get_height(shm);
        return;
    }
    if (DmabufInfo info{}; dmabuf_buffer_info(buffer, info)) {
        w = info.width;
        h = info.height;
    }
}
} // namespace

// ---- client buffer lifetime -------------------------------------------------
//
// A wl_buffer belongs to the CLIENT, and clients destroy buffers while the
// compositor still holds them: every toolkit drops its whole swapchain when a
// window is resized, hidden, or re-shown. Keeping the raw wl_resource* means
// the next `wl_buffer.release` — or the next readback — touches freed memory.
// So every buffer we reference carries a destroy subscription.

struct Surface::BufferWatch {
    wl_listener listener{}; // must stay first: we recover the watch from it
    Surface* surface = nullptr;
    wl_resource* buffer = nullptr;

    ~BufferWatch() {
        // After libwayland's final emit the link is re-initialised, so removing
        // it again is harmless; before we ever attached it, it is null.
        if (listener.link.prev != nullptr) {
            wl_list_remove(&listener.link);
        }
    }
};

namespace {
void on_buffer_destroy(wl_listener* listener, void*) {
    auto* watch = reinterpret_cast<Surface::BufferWatch*>(
        reinterpret_cast<char*>(listener) - offsetof(Surface::BufferWatch, listener));
    watch->surface->forget_buffer(watch->buffer);
}
} // namespace

void Surface::watch_buffer(wl_resource* buffer) {
    if (buffer == nullptr) {
        return;
    }
    for (const std::unique_ptr<BufferWatch>& watch : buffer_watches_) {
        if (watch->buffer == buffer) {
            return; // already subscribed
        }
    }
    auto watch = std::make_unique<BufferWatch>();
    watch->surface = this;
    watch->buffer = buffer;
    watch->listener.notify = on_buffer_destroy;
    wl_resource_add_destroy_listener(buffer, &watch->listener);
    buffer_watches_.push_back(std::move(watch));
}

void Surface::prune_buffer_watches() {
    std::erase_if(buffer_watches_, [this](const std::unique_ptr<BufferWatch>& watch) {
        return watch->buffer != current_buffer_ && watch->buffer != pending_.buffer &&
               watch->buffer != cached_.buffer;
    });
}

void Surface::forget_buffer(wl_resource* buffer) noexcept {
    // A pending/cached attach of a now-dead buffer degrades to "attach null",
    // which is exactly what the surface should show: nothing.
    if (pending_.buffer == buffer) {
        pending_.buffer = nullptr;
    }
    if (cached_.buffer == buffer) {
        cached_.buffer = nullptr;
    }
    if (current_buffer_ == buffer) {
        current_buffer_ = nullptr;
        buffer_width_ = 0;
        buffer_height_ = 0;
    }
    std::erase_if(buffer_watches_, [buffer](const std::unique_ptr<BufferWatch>& watch) {
        return watch->buffer == buffer;
    });
}

void Surface::set_pending_buffer(wl_resource* buffer) {
    pending_.buffer = buffer;
    pending_.buffer_dirty = true;
    watch_buffer(buffer);
}

Surface::Surface(wl_resource* resource) noexcept : resource_(resource) {}

Surface::~Surface() {
    SurfaceDestroy event{*this};
    destroy.emit(event);
    // Orphan our children and unlink from our parent so no dangling edges remain.
    for (Surface* child : below_) {
        child->parent_ = nullptr;
    }
    for (Surface* child : above_) {
        child->parent_ = nullptr;
    }
    if (parent_ != nullptr) {
        std::erase(parent_->below_, this);
        std::erase(parent_->above_, this);
        parent_ = nullptr;
    }
}

bool Surface::effective_sync() const noexcept {
    for (const Surface* s = this; s->parent_ != nullptr; s = s->parent_) {
        if (s->sub_sync_) {
            return true;
        }
    }
    return false;
}

void Surface::merge_state(State& into, State&& from) {
    if (from.buffer_dirty) {
        // A buffer superseded before it was ever applied is released right away.
        if (into.buffer_dirty && into.buffer != nullptr && into.buffer != from.buffer) {
            wl_buffer_send_release(into.buffer);
        }
        into.buffer = from.buffer;
        into.buffer_dirty = true;
    }
    into.frame_callbacks.insert(into.frame_callbacks.end(), from.frame_callbacks.begin(),
                                from.frame_callbacks.end());
    from.frame_callbacks.clear();
}

void Surface::apply_commit() {
    if (parent_ != nullptr && effective_sync()) {
        // Sync subsurface: state is cached until the parent commits.
        merge_state(cached_, std::move(pending_));
        has_cached_ = true;
        pending_ = State{};
        prune_buffer_watches();
        return;
    }
    State state = std::move(pending_);
    pending_ = State{};
    commit_state(std::move(state));
}

void Surface::commit_state(State state) {
    if (state.buffer_dirty) {
        // Release the buffer we're replacing so the client can reuse it. We copy
        // buffer contents at render time, so we only need to hold the current one.
        if (current_buffer_ != nullptr && current_buffer_ != state.buffer) {
            wl_buffer_send_release(current_buffer_);
        }
        current_buffer_ = state.buffer;
        buffer_size(current_buffer_, buffer_width_, buffer_height_);
    }
    prune_buffer_watches();
    SurfaceCommit event{*this};
    commit.emit(event);

    // Ack frame callbacks so the client draws its next frame.
    // TODO: fired on commit rather than on presentation — fine for now; pace
    // via the output frame if a client spins.
    const uint32_t time = now_ms();
    for (wl_resource* cb : state.frame_callbacks) {
        wl_callback_send_done(cb, time);
        wl_resource_destroy(cb);
    }

    // A parent commit applies the whole synced subtree, including subsurface
    // positions (which are parent state).
    const std::vector<Surface*> below = below_;
    const std::vector<Surface*> above = above_;
    for (Surface* child : below) {
        child->parent_committed();
    }
    for (Surface* child : above) {
        child->parent_committed();
    }
}

void Surface::parent_committed() {
    apply_pending_position();
    if (has_cached_) {
        State state = std::move(cached_);
        cached_ = State{};
        has_cached_ = false;
        commit_state(std::move(state)); // recurses into our own children
        return;
    }
    const std::vector<Surface*> below = below_;
    const std::vector<Surface*> above = above_;
    for (Surface* child : below) {
        child->parent_committed();
    }
    for (Surface* child : above) {
        child->parent_committed();
    }
}

void Surface::apply_pending_position() noexcept {
    if (sub_position_dirty_) {
        sub_x_ = pending_sub_x_;
        sub_y_ = pending_sub_y_;
        sub_position_dirty_ = false;
    }
}

bool Surface::is_ancestor_of(const Surface& other) const noexcept {
    for (const Surface* s = other.parent_; s != nullptr; s = s->parent_) {
        if (s == this) {
            return true;
        }
    }
    return false;
}

bool Surface::sub_attach(Surface& parent) {
    if (&parent == this || is_ancestor_of(parent)) {
        return false; // would make a cycle
    }
    sub_detach();
    parent_ = &parent;
    parent.above_.push_back(this);
    sub_sync_ = true;
    return true;
}

void Surface::sub_detach() {
    if (parent_ == nullptr) {
        return;
    }
    std::erase(parent_->below_, this);
    std::erase(parent_->above_, this);
    parent_ = nullptr;
    sub_x_ = sub_y_ = 0;
    sub_position_dirty_ = false;
    // Cached sync state has no parent to release it any more — apply it now.
    if (has_cached_) {
        State state = std::move(cached_);
        cached_ = State{};
        has_cached_ = false;
        commit_state(std::move(state));
    }
}

void Surface::sub_set_position(int x, int y) noexcept {
    pending_sub_x_ = x;
    pending_sub_y_ = y;
    sub_position_dirty_ = true;
}

void Surface::sub_set_sync(bool sync) noexcept {
    sub_sync_ = sync;
    if (!sync && has_cached_ && !effective_sync()) {
        State state = std::move(cached_);
        cached_ = State{};
        has_cached_ = false;
        commit_state(std::move(state));
    }
}

bool Surface::sub_place(Surface& sibling, bool above) {
    if (parent_ == nullptr) {
        return false;
    }
    Surface* parent = parent_;
    std::vector<Surface*>& target = above ? parent->above_ : parent->below_;
    if (&sibling == parent) {
        // Relative to the parent itself: move into the above/below list, at the
        // edge nearest the parent.
        std::erase(parent->below_, this);
        std::erase(parent->above_, this);
        if (above) {
            target.insert(target.begin(), this);
        } else {
            target.push_back(this);
        }
        return true;
    }
    if (sibling.parent_ != parent) {
        return false;
    }
    std::erase(parent->below_, this);
    std::erase(parent->above_, this);
    for (std::vector<Surface*>* list : {&parent->below_, &parent->above_}) {
        auto it = std::find(list->begin(), list->end(), &sibling);
        if (it != list->end()) {
            list->insert(above ? it + 1 : it, this);
            return true;
        }
    }
    return false; // sibling vanished between the checks; nothing sensible to do
}

void Surface::collect_tree(std::vector<SurfaceAt>& out, int ox, int oy) {
    for (Surface* child : below_) {
        child->collect_tree(out, ox + child->sub_x_, oy + child->sub_y_);
    }
    out.push_back(SurfaceAt{this, ox, oy});
    for (Surface* child : above_) {
        child->collect_tree(out, ox + child->sub_x_, oy + child->sub_y_);
    }
}

std::vector<SurfaceAt> Surface::surface_tree() {
    std::vector<SurfaceAt> out;
    collect_tree(out, 0, 0);
    return out;
}

std::optional<SurfaceAt> Surface::surface_at(double sx, double sy) {
    const std::vector<SurfaceAt> tree = surface_tree();
    for (auto it = tree.rbegin(); it != tree.rend(); ++it) { // front-to-back
        const Surface* s = it->surface;
        if (s->buffer_width_ <= 0 || s->buffer_height_ <= 0) {
            continue;
        }
        if (sx >= it->x && sy >= it->y && sx < it->x + s->buffer_width_ &&
            sy < it->y + s->buffer_height_) {
            return *it;
        }
    }
    return std::nullopt;
}

namespace {

Surface* surface_of(wl_resource* resource) {
    return static_cast<Surface*>(wl_resource_get_user_data(resource));
}

void surface_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void surface_attach(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t, int32_t) {
    surface_of(resource)->set_pending_buffer(buffer);
}
void surface_commit(wl_client*, wl_resource* resource) {
    surface_of(resource)->apply_commit();
}
void surface_frame(wl_client* client, wl_resource* resource, uint32_t callback_id) {
    wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (cb != nullptr) {
        surface_of(resource)->add_frame_callback(cb);
    }
}
// Requests we accept but don't act on yet (damage tracking / regions / transforms).
void surface_noop_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void surface_noop_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_noop_i32(wl_client*, wl_resource*, int32_t) {}
void surface_noop_offset(wl_client*, wl_resource*, int32_t, int32_t) {}

// TODO: all requests wired so real clients don't hit a null slot; damage /
// regions / transform / scale / offset are accepted no-ops for now.
constexpr struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_request,
    .attach = surface_attach,
    .damage = surface_noop_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_noop_region,
    .set_input_region = surface_noop_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_noop_i32,
    .set_buffer_scale = surface_noop_i32,
    .damage_buffer = surface_noop_damage,
    .offset = surface_noop_offset,
};

void surface_resource_destroy(wl_resource* resource) {
    delete surface_of(resource);
}

void compositor_create_surface(wl_client* client, wl_resource* compositor_resource, uint32_t id) {
    auto* impl = static_cast<Compositor::Impl*>(wl_resource_get_user_data(compositor_resource));
    wl_resource* resource = wl_resource_create(
        client, &wl_surface_interface, wl_resource_get_version(compositor_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* surface = new Surface{resource};
    wl_resource_set_implementation(resource, &surface_impl, surface, surface_resource_destroy);

    NewSurface event{*surface};
    impl->new_surface.emit(event);
}

// Minimal wl_region: all requests wired (add/subtract accepted no-ops) so clients
// that build opaque/input regions don't hit a null slot.
// TODO: track the region geometry when input/opaque regions actually matter.
void region_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
void region_add(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void region_subtract(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
constexpr struct wl_region_interface region_impl = {
    .destroy = region_destroy_request,
    .add = region_add,
    .subtract = region_subtract,
};

void compositor_create_region(wl_client* client, wl_resource* compositor_resource, uint32_t id) {
    wl_resource* resource = wl_resource_create(
        client, &wl_region_interface, wl_resource_get_version(compositor_resource), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &region_impl, nullptr, nullptr);
}

constexpr struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

void compositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_compositor_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, data, nullptr);
}

} // namespace

Compositor::Compositor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Compositor::~Compositor() = default;
Compositor::Compositor(Compositor&&) noexcept = default;
Compositor& Compositor::operator=(Compositor&&) noexcept = default;

Result<Compositor> Compositor::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    // Version 4: covers attach/damage/commit/scale/transform; enough for now.
    impl->global = wl_global_create(impl->display, &wl_compositor_interface, 4, impl.get(),
                                    compositor_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wl_compositor) failed");
    }
    return Compositor{std::move(impl)};
}

Signal<NewSurface>& Compositor::new_surface() noexcept {
    return impl_->new_surface;
}

} // namespace luminaria
