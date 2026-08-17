// luminaria/session_lock.cppm — ext_session_lock_manager_v1: the lock screen.
//
// A separate, unprivileged-looking client draws the lock screen, but the
// guarantee is the compositor's: from the moment it answers `locked`, NOTHING
// else may be shown or receive input until that same client says otherwise.
// The protocol exists because the old way — a normal window that grabs the
// keyboard — fails open: the locker crashes and the desktop is on display.
//
// Here it fails CLOSED, and that is the one rule this file is written around:
//
//   * `SessionLock::send_locked()` is the compositor's promise that the screen
//     is covered. Send it only once every output has a lock surface with a
//     buffer on it, or you have blanked the ones that do not.
//   * When the lock object goes away, `SessionLockDestroy::unlocked` says
//     whether the client asked (`unlock_and_destroy`) or simply died. If it
//     died, the session STAYS locked — keep showing black and taking no input
//     until a new lock client arrives and locks again. This library will not
//     unlock on your behalf.
//
// The compositor's part beyond that is small: `SessionLockManager::locked()`
// gates what it draws and where input goes, and each `SessionLockSurface` is
// configured to exactly one output's size, drawn on top of everything, and
// given keyboard focus.
//
// A second lock while one is held is refused with `finished` — one lock at a
// time is the whole model.

module;

#include <cstdint>

#include <typeinfo>
#include <wayland-server-core.h>
#include "ext-session-lock-v1-protocol.h"

export module luminaria.desktop:session_lock;

import std;

import luminaria;

export namespace luminaria {

class SessionLock;
class SessionLockSurface;

struct NewSessionLock {
    SessionLock& lock;
};
/// Fired for each output surface the lock client creates. Configure it with the
/// output's size; it may not draw before you do.
struct NewSessionLockSurface {
    SessionLockSurface& lock_surface;
};
/// The client committed a buffer for a size it was configured to. This is the
/// surface becoming showable — a lock is only real once every output has one.
struct SessionLockSurfaceMap {
    SessionLockSurface& lock_surface;
};
struct SessionLockSurfaceDestroy {
    SessionLockSurface& lock_surface;
};
/// The lock object is going away. `unlocked` distinguishes the two cases that
/// look identical on the wire and could not be more different in effect:
/// true — the client called `unlock_and_destroy`, show the desktop again;
/// false — the client died holding the lock, so STAY LOCKED.
struct SessionLockDestroy {
    SessionLock& lock;
    bool unlocked;
};

/// One ext_session_lock_surface_v1: the lock client's window for one output.
/// Owned by its resource; address stable, so signals may capture a reference.
class SessionLockSurface {
public:
    virtual ~SessionLockSurface() = default;
    SessionLockSurface(const SessionLockSurface&) = delete;
    SessionLockSurface& operator=(const SessionLockSurface&) = delete;

    Signal<SessionLockSurfaceMap> map;
    Signal<SessionLockSurfaceDestroy> destroy;

    /// The wl_surface to draw. Never null while this object is alive.
    [[nodiscard]] virtual Surface& surface() noexcept = 0;
    /// The client's wl_output resource this surface belongs to. Match it with
    /// `OutputGlobal::resource_for(client)` to find which output it is.
    [[nodiscard]] virtual wl_resource* output_resource() const noexcept = 0;

    /// Tell the client how big to draw, in surface coordinates. It must answer
    /// with exactly this size — a lock surface that could pick its own would
    /// leave a strip of desktop showing. Returns the configure serial.
    virtual std::uint32_t configure(std::uint32_t width, std::uint32_t height) = 0;

    /// True once the client has acked a configure and committed a buffer of the
    /// configured size: there is something to show.
    [[nodiscard]] virtual bool mapped() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;

protected:
    SessionLockSurface() = default;
};

/// One ext_session_lock_v1: a client's claim on the session. Owned by its
/// resource; address stable for its lifetime.
class SessionLock {
public:
    virtual ~SessionLock() = default;
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    Signal<NewSessionLockSurface> new_surface;
    Signal<SessionLockDestroy> destroy;

    /// "The screen is covered." After this the client may assume nothing else is
    /// visible — so do not send it before that is true.
    virtual void send_locked() = 0;
    /// "You do not have the lock." Refusal, or a lock we can no longer honour.
    /// The client is expected to destroy itself; it is not the screen's keeper.
    virtual void send_finished() = 0;

    /// Whether `send_locked()` has been sent for this lock.
    [[nodiscard]] virtual bool locked() const noexcept = 0;
    /// The lock surfaces created so far, in creation order.
    [[nodiscard]] virtual const std::vector<SessionLockSurface*>& surfaces() const noexcept = 0;

protected:
    SessionLock() = default;
};

/// The ext_session_lock_manager_v1 global (version 1). Move-only;
/// pointer-stable state.
class SessionLockManager {
public:
    [[nodiscard]] static Result<SessionLockManager> create(Display& display);

    ~SessionLockManager();
    SessionLockManager(SessionLockManager&&) noexcept;
    SessionLockManager& operator=(SessionLockManager&&) noexcept;
    SessionLockManager(const SessionLockManager&) = delete;
    SessionLockManager& operator=(const SessionLockManager&) = delete;

    [[nodiscard]] Signal<NewSessionLock>& new_lock() noexcept;

    /// The lock currently held, or null. Null does NOT mean "show the desktop":
    /// a lock client that died leaves this null with the session still locked,
    /// which is what `session_locked()` remembers.
    [[nodiscard]] SessionLock* current() noexcept;

    /// The compositor-side latch: true from the first `send_locked()` until a
    /// client's orderly `unlock_and_destroy`. Gate drawing and input on this,
    /// not on `current()`.
    [[nodiscard]] bool session_locked() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit SessionLockManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements ext_session_lock_manager_v1 (version 1). The protocol errors are
// all worth posting here: each one is a lock client that would otherwise leave
// a hole in the screen it claims to be covering.

namespace luminaria {

// Not in an anonymous namespace: SessionLockManager::Impl holds these.
class LockImpl;
class LockSurfaceImpl;

struct SessionLockManager::Impl {
    WlGlobal global;
    LockImpl* current = nullptr;
    bool session_locked = false;

    Signal<NewSessionLock> new_lock;
};

using SlMgr = SessionLockManager::Impl;

class LockImpl final : public SessionLock {
public:
    LockImpl(SlMgr* mgr, wl_resource* resource) : mgr_(mgr), resource_(resource) {}

    void send_locked() override {
        if (locked_ || finished_) {
            return;
        }
        locked_ = true;
        mgr_->session_locked = true;
        ext_session_lock_v1_send_locked(resource_);
    }

    void send_finished() override {
        if (finished_) {
            return;
        }
        finished_ = true;
        ext_session_lock_v1_send_finished(resource_);
    }

    [[nodiscard]] bool locked() const noexcept override { return locked_; }
    [[nodiscard]] const std::vector<SessionLockSurface*>& surfaces() const noexcept override {
        return surfaces_;
    }

    SlMgr* mgr_ = nullptr;
    wl_resource* resource_ = nullptr;
    bool locked_ = false;
    bool finished_ = false;
    bool unlocking_ = false; // set by unlock_and_destroy, read by the destroy hook
    std::vector<SessionLockSurface*> surfaces_;
    std::vector<wl_resource*> outputs_;    // one lock surface per wl_output
};

class LockSurfaceImpl final : public SessionLockSurface {
public:
    LockSurfaceImpl(LockImpl* lock, Surface& surface, wl_resource* output, wl_resource* resource)
        : lock_(lock), surface_(&surface), output_(output), resource_(resource) {
        on_commit_ = surface.commit.connect([this](SurfaceCommit&) { check_commit(); });
        on_destroy_ = surface.destroy.connect([this](SurfaceDestroy&) {
            // The wl_surface died under its role object. Detach so nothing
            // touches it afterwards; the role object stays until the client
            // destroys it, answering requests as inert.
            surface_ = nullptr;
        });
    }

    Surface& surface() noexcept override { return *surface_; }
    [[nodiscard]] wl_resource* output_resource() const noexcept override { return output_; }
    [[nodiscard]] bool mapped() const noexcept override { return mapped_; }
    [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }

    std::uint32_t configure(std::uint32_t width, std::uint32_t height) override {
        width_ = width;
        height_ = height;
        const std::uint32_t serial = ++serial_;
        pending_serials_.push_back(serial);
        ext_session_lock_surface_v1_send_configure(resource_, serial, width, height);
        return serial;
    }

    /// The three commit-time errors. A lock surface that draws the wrong size,
    /// or nothing at all, is a gap in the lock screen — so they are protocol
    /// errors rather than something the compositor has to notice.
    void check_commit() {
        if (surface_ == nullptr) {
            return;
        }
        if (!acked_) {
            wl_resource_post_error(resource_,
                                   EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK,
                                   "committed before acking the first configure");
            return;
        }
        if (!surface_->has_buffer()) {
            wl_resource_post_error(resource_, EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER,
                                   "a lock surface must always have a buffer");
            return;
        }
        const auto w = static_cast<std::uint32_t>(surface_->surface_width());
        const auto h = static_cast<std::uint32_t>(surface_->surface_height());
        if (w != width_ || h != height_) {
            wl_resource_post_error(resource_,
                                   EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH,
                                   "committed a buffer that is not the configured size");
            return;
        }
        if (!mapped_) {
            mapped_ = true;
            SessionLockSurfaceMap event{*this};
            map.emit(event);
        }
    }

    LockImpl* lock_ = nullptr;
    Surface* surface_ = nullptr;
    wl_resource* output_ = nullptr;
    wl_resource* resource_ = nullptr;
    std::uint32_t serial_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool acked_ = false;
    bool mapped_ = false;
    std::vector<std::uint32_t> pending_serials_;
    Signal<SurfaceCommit>::Connection on_commit_;
    Signal<SurfaceDestroy>::Connection on_destroy_;
};

namespace {

// --- ext_session_lock_surface_v1 ---

void lock_surface_ack_configure(wl_client*, wl_resource* resource, uint32_t serial) {
    auto* ls = static_cast<LockSurfaceImpl*>(wl_resource_get_user_data(resource));
    auto it = std::ranges::find(ls->pending_serials_, serial);
    if (it == ls->pending_serials_.end()) {
        wl_resource_post_error(resource, EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
                               "acked a serial that was never configured");
        return;
    }
    // Everything older is superseded, exactly like xdg_surface.ack_configure.
    ls->pending_serials_.erase(ls->pending_serials_.begin(), it + 1);
    ls->acked_ = true;
}

constexpr struct ext_session_lock_surface_v1_interface lock_surface_impl = {
    .destroy = resource_destroy_request,
    .ack_configure = lock_surface_ack_configure,
};

void lock_surface_resource_destroy(wl_resource* resource) {
    auto* ls = static_cast<LockSurfaceImpl*>(wl_resource_get_user_data(resource));
    SessionLockSurfaceDestroy event{*ls};
    ls->destroy.emit(event);
    std::erase(ls->lock_->surfaces_, static_cast<SessionLockSurface*>(ls));
    std::erase(ls->lock_->outputs_, ls->output_);
    delete ls;
}

// --- ext_session_lock_v1 ---

void lock_destroy_request(wl_client*, wl_resource* resource) {
    auto* lock = static_cast<LockImpl*>(wl_resource_get_user_data(resource));
    if (lock->locked_) {
        // Destroying a lock that was granted, without unlocking: the client is
        // trying to walk away from a locked screen.
        wl_resource_post_error(resource, EXT_SESSION_LOCK_V1_ERROR_INVALID_DESTROY,
                               "destroy on a locked session; use unlock_and_destroy");
        return;
    }
    wl_resource_destroy(resource);
}

void lock_unlock_and_destroy(wl_client*, wl_resource* resource) {
    auto* lock = static_cast<LockImpl*>(wl_resource_get_user_data(resource));
    if (!lock->locked_) {
        wl_resource_post_error(resource, EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK,
                               "unlock_and_destroy before the lock was granted");
        return;
    }
    lock->mgr_->session_locked = false;
    lock->unlocking_ = true;
    wl_resource_destroy(resource);
}

void lock_get_lock_surface(wl_client* client, wl_resource* lock_resource, uint32_t id,
                           wl_resource* surface_resource, wl_resource* output_resource) {
    auto* lock = static_cast<LockImpl*>(wl_resource_get_user_data(lock_resource));
    Surface* surface = surface_from_resource(surface_resource);
    if (surface == nullptr) {
        wl_resource_post_error(lock_resource, EXT_SESSION_LOCK_V1_ERROR_ROLE,
                               "not a wl_surface");
        return;
    }
    if (surface->has_buffer()) {
        // "The surface must not have a buffer committed" — a lock surface that
        // arrives already drawn was drawn before we could size it.
        wl_resource_post_error(lock_resource, EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED,
                               "the wl_surface already has a committed buffer");
        return;
    }
    if (std::ranges::find(lock->outputs_, output_resource) != lock->outputs_.end()) {
        wl_resource_post_error(lock_resource, EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
                               "this output already has a lock surface");
        return;
    }
    wl_resource* resource = wl_resource_create(client, &ext_session_lock_surface_v1_interface,
                                               wl_resource_get_version(lock_resource),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_resource_post_no_memory(lock_resource);
        return;
    }
    auto* ls = new LockSurfaceImpl(lock, *surface, output_resource, resource);
    wl_resource_set_implementation(resource, &lock_surface_impl, ls,
                                   lock_surface_resource_destroy);
    lock->surfaces_.push_back(ls);
    lock->outputs_.push_back(output_resource);

    NewSessionLockSurface event{*ls};
    lock->new_surface.emit(event);
}

constexpr struct ext_session_lock_v1_interface lock_impl = {
    .destroy = lock_destroy_request,
    .get_lock_surface = lock_get_lock_surface,
    .unlock_and_destroy = lock_unlock_and_destroy,
};

void lock_resource_destroy(wl_resource* resource) {
    auto* lock = static_cast<LockImpl*>(wl_resource_get_user_data(resource));
    // The lock surfaces are the client's; libwayland destroys them with the
    // client, and each unhooks itself from `lock->surfaces_` as it goes.
    SessionLockDestroy event{*lock, lock->unlocking_};
    lock->destroy.emit(event);
    if (lock->mgr_->current == lock) {
        lock->mgr_->current = nullptr;
    }
    delete lock;
}

// --- ext_session_lock_manager_v1 ---

void manager_lock(wl_client* client, wl_resource* manager, uint32_t id) {
    auto* mgr = static_cast<SlMgr*>(wl_resource_get_user_data(manager));
    wl_resource* resource = wl_resource_create(client, &ext_session_lock_v1_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    auto* lock = new LockImpl(mgr, resource);
    wl_resource_set_implementation(resource, &lock_impl, lock, lock_resource_destroy);
    if (mgr->current != nullptr) {
        // One lock at a time. The loser is told so rather than left waiting.
        lock->send_finished();
        return;
    }
    mgr->current = lock;

    NewSessionLock event{*lock};
    mgr->new_lock.emit(event);
}

constexpr struct ext_session_lock_manager_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .lock = manager_lock,
};

} // namespace

SessionLockManager::SessionLockManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SessionLockManager::~SessionLockManager() = default;
SessionLockManager::SessionLockManager(SessionLockManager&&) noexcept = default;
SessionLockManager& SessionLockManager::operator=(SessionLockManager&&) noexcept = default;

Result<SessionLockManager> SessionLockManager::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    auto global = create_wl_global<&ext_session_lock_manager_v1_interface,
                                   default_bind<&ext_session_lock_manager_v1_interface,
                                                &manager_impl>>(display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return SessionLockManager{std::move(impl)};
}

Signal<NewSessionLock>& SessionLockManager::new_lock() noexcept {
    return impl_->new_lock;
}
SessionLock* SessionLockManager::current() noexcept {
    return impl_->current;
}
bool SessionLockManager::session_locked() const noexcept {
    return impl_->session_locked;
}

} // namespace luminaria
