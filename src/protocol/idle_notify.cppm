// luminaria/protocol/idle_notify.cppm — ext_idle_notifier_v1: "tell me when the
// user has stopped touching this machine".
//
// The mirror image of idle-inhibit. There the client says "keep the screen on";
// here it asks to be told after N milliseconds of no input, so a screen locker
// can lock, a chat client can go away, a dimmer can dim. Each notification is an
// independent timer: a locker asking for 10 minutes and a dimmer asking for 30
// seconds both get exactly what they asked for.
//
// The library cannot see input — the compositor routes that — so activity comes
// in through `notify_activity()`, called from wherever key/pointer/touch events
// are already being handled. Nothing idles until something calls it.
//
// Version 2 adds `get_input_idle_notification`, which differs in exactly one
// way: it ignores idle inhibitors. A notification made that way keeps counting
// down while a video player holds the screen awake; an ordinary one does not.
// Wire `IdleInhibitManager::changed` into `set_inhibited()` and both behave.

module;


#include <typeinfo>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "ext-idle-notify-v1-protocol.h"

export module luminaria:idle_notify;

import std;

import :display;
import :event_loop;
import :expected;
import :protocol_helper;
import :signal;

export namespace luminaria {

class Display;
class IdleNotification;

struct NewIdleNotification {
    IdleNotification& notification;
};
struct IdleNotificationDestroy {
    IdleNotification& notification;
};
/// Fired whenever a notification crosses into or out of the idle state, so a
/// compositor can drive dimming or DPMS off the same accounting the clients see.
struct IdleStateChange {
    IdleNotification& notification;
    bool idled;
};

/// One ext_idle_notification_v1. Owned by its resource; the address is stable
/// for its lifetime, so signals may capture `IdleNotification&`.
class IdleNotification {
public:
    virtual ~IdleNotification() = default;
    IdleNotification(const IdleNotification&) = delete;
    IdleNotification& operator=(const IdleNotification&) = delete;

    Signal<IdleNotificationDestroy> destroy;

    /// What the client asked for, in milliseconds. Zero is legal and means
    /// "as soon as the seat is idle at all".
    [[nodiscard]] virtual std::uint32_t timeout_ms() const noexcept = 0;

    /// True between `idled` and `resumed`.
    [[nodiscard]] virtual bool idled() const noexcept = 0;

    /// Created with `get_input_idle_notification` (version 2): counts raw input
    /// only and is not held off by idle inhibitors.
    [[nodiscard]] virtual bool input_only() const noexcept = 0;

protected:
    IdleNotification() = default;
};

/// The ext_idle_notifier_v1 global (version 2). Move-only; pointer-stable state.
class IdleNotifier {
public:
    /// Needs the event loop because every notification is a timer.
    [[nodiscard]] static Result<IdleNotifier> create(Display& display, EventLoop loop);

    ~IdleNotifier();
    IdleNotifier(IdleNotifier&&) noexcept;
    IdleNotifier& operator=(IdleNotifier&&) noexcept;
    IdleNotifier(const IdleNotifier&) = delete;
    IdleNotifier& operator=(const IdleNotifier&) = delete;

    [[nodiscard]] Signal<NewIdleNotification>& new_notification() noexcept;
    [[nodiscard]] Signal<IdleStateChange>& state_change() noexcept;

    /// The user did something. Call this from the compositor's key, pointer and
    /// touch handling — every notification restarts its countdown, and anything
    /// currently idle is told it resumed.
    ///
    /// Cheap enough to call on every event: it walks the live notifications and
    /// re-arms a timer each, and there are only ever a handful of those.
    void notify_activity();

    /// Whether an idle inhibitor is currently held. While true, notifications
    /// made with `get_idle_notification` neither idle nor stay idle; the
    /// input-only ones (version 2) are unaffected. Wire this to
    /// `IdleInhibitManager::changed`.
    void set_inhibited(bool inhibited);
    [[nodiscard]] bool inhibited() const noexcept;

    /// Live notifications, and how many of them are currently idle.
    [[nodiscard]] std::size_t notification_count() const noexcept;
    [[nodiscard]] std::size_t idled_count() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit IdleNotifier(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements ext_idle_notifier_v1 (version 2).
//
// One timer per notification. libwayland's timer sources are one-shot and
// `wl_event_source_timer_update(src, 0)` *disarms* rather than firing
// immediately, so a client asking for a 0 ms timeout is armed at 1 ms — the
// nearest thing to "as soon as the seat is idle" that the loop can express.

namespace luminaria {

// Not in an anonymous namespace: IdleNotifier::Impl holds these, and a
// module-linkage class may not have a member naming an internal-linkage type.
class NotificationImpl;

struct IdleNotifier::Impl {
    WlGlobal global;
    EventLoop loop;
    std::vector<NotificationImpl*> notifications;
    bool inhibited = false;

    Signal<NewIdleNotification> new_notification;
    Signal<IdleStateChange> state_change;
};

using InMgr = IdleNotifier::Impl;

class NotificationImpl final : public IdleNotification {
public:
    NotificationImpl(InMgr* mgr, wl_resource* resource, std::uint32_t timeout, bool input_only)
        : mgr_(mgr), resource_(resource), timeout_(timeout), input_only_(input_only) {
        timer_ = mgr_->loop.add_timer([this] { fire(); });
        rearm();
    }

    [[nodiscard]] std::uint32_t timeout_ms() const noexcept override { return timeout_; }
    [[nodiscard]] bool idled() const noexcept override { return idled_; }
    [[nodiscard]] bool input_only() const noexcept override { return input_only_; }

    /// Held off by an inhibitor? Input-only notifications never are.
    [[nodiscard]] bool held() const noexcept { return mgr_->inhibited && !input_only_; }

    /// Restart the countdown. Anything already idle resumes first, because the
    /// client's view is a strict idled/resumed alternation.
    void rearm() {
        resume();
        if (held()) {
            timer_.disarm();
            return;
        }
        timer_.arm(timeout_ == 0 ? 1 : timeout_);
    }

    /// An inhibitor appeared: stop counting and undo any idle we reported.
    void hold() {
        resume();
        timer_.disarm();
    }

    void resume() {
        if (!idled_) {
            return;
        }
        idled_ = false;
        ext_idle_notification_v1_send_resumed(resource_);
        IdleStateChange event{*this, false};
        mgr_->state_change.emit(event);
    }

    void fire() {
        if (idled_ || held()) {
            return;
        }
        idled_ = true;
        ext_idle_notification_v1_send_idled(resource_);
        IdleStateChange event{*this, true};
        mgr_->state_change.emit(event);
    }

    InMgr* mgr_ = nullptr;
    wl_resource* resource_ = nullptr;
    std::uint32_t timeout_ = 0;
    bool input_only_ = false;
    bool idled_ = false;
    EventSource timer_;
};

namespace {

const struct ext_idle_notification_v1_interface notification_impl = {
    .destroy = resource_destroy_request,
};

void notification_resource_destroy(wl_resource* resource) {
    auto* n = static_cast<NotificationImpl*>(wl_resource_get_user_data(resource));
    IdleNotificationDestroy event{*n};
    n->destroy.emit(event);
    auto& live = n->mgr_->notifications;
    live.erase(std::remove(live.begin(), live.end(), n), live.end());
    delete n;
}

/// Both manager requests land here; `input_only` is the only difference.
void make_notification(wl_client* client, wl_resource* manager, std::uint32_t id,
                       std::uint32_t timeout, bool input_only) {
    auto* mgr = static_cast<InMgr*>(wl_resource_get_user_data(manager));
    wl_resource* resource = wl_resource_create(client, &ext_idle_notification_v1_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(id));
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* n = new NotificationImpl{mgr, resource, timeout, input_only};
    wl_resource_set_implementation(resource, &notification_impl, n, notification_resource_destroy);
    mgr->notifications.push_back(n);
    NewIdleNotification event{*n};
    mgr->new_notification.emit(event);
}

// The wl_seat argument is accepted and ignored: this library has a single seat,
// so "which seat" and "which client" are the same question, and routing is by
// client already.
void manager_get_idle_notification(wl_client* client, wl_resource* manager, std::uint32_t id,
                                   std::uint32_t timeout, wl_resource* /*seat*/) {
    make_notification(client, manager, id, timeout, false);
}

void manager_get_input_idle_notification(wl_client* client, wl_resource* manager, std::uint32_t id,
                                         std::uint32_t timeout, wl_resource* /*seat*/) {
    make_notification(client, manager, id, timeout, true);
}

const struct ext_idle_notifier_v1_interface manager_impl = {
    .destroy = resource_destroy_request,
    .get_idle_notification = manager_get_idle_notification,
    .get_input_idle_notification = manager_get_input_idle_notification,
};

} // namespace

IdleNotifier::IdleNotifier(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IdleNotifier::~IdleNotifier() = default;
IdleNotifier::IdleNotifier(IdleNotifier&&) noexcept = default;
IdleNotifier& IdleNotifier::operator=(IdleNotifier&&) noexcept = default;

Result<IdleNotifier> IdleNotifier::create(Display& display, EventLoop loop) {
    auto impl = std::make_unique<Impl>();
    impl->loop = loop;
    auto global = create_wl_global<&ext_idle_notifier_v1_interface,
                                   default_bind<&ext_idle_notifier_v1_interface,
                                                &manager_impl>>(display, 2, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return IdleNotifier{std::move(impl)};
}

Signal<NewIdleNotification>& IdleNotifier::new_notification() noexcept {
    return impl_->new_notification;
}

Signal<IdleStateChange>& IdleNotifier::state_change() noexcept { return impl_->state_change; }

void IdleNotifier::notify_activity() {
    // Copy: resume() emits, and a handler is allowed to destroy a notification.
    const std::vector<NotificationImpl*> live = impl_->notifications;
    for (NotificationImpl* n : live) {
        n->rearm();
    }
}

void IdleNotifier::set_inhibited(bool inhibited) {
    if (impl_->inhibited == inhibited) {
        return;
    }
    impl_->inhibited = inhibited;
    const std::vector<NotificationImpl*> live = impl_->notifications;
    for (NotificationImpl* n : live) {
        if (n->input_only()) {
            continue; // version 2: deliberately blind to inhibitors
        }
        if (inhibited) {
            n->hold();
        } else {
            n->rearm(); // the countdown restarts from now, not from before
        }
    }
}

bool IdleNotifier::inhibited() const noexcept { return impl_->inhibited; }

std::size_t IdleNotifier::notification_count() const noexcept {
    return impl_->notifications.size();
}

std::size_t IdleNotifier::idled_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(impl_->notifications.begin(), impl_->notifications.end(),
                      [](const NotificationImpl* n) { return n->idled(); }));
}

} // namespace luminaria
