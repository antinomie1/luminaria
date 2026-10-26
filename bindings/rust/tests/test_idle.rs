//! idle-inhibit and ext-idle-notify events, driven by a real client: an
//! inhibitor counted while its surface is visible, and a notification that
//! idles on its timer and resumes on activity.

#[macro_use]
mod common;

use common::{drain, pump, ClientSide, Peer};
use luminaria::core::Display;
use luminaria::protocol::{Compositor, IdleInhibitEvent, IdleInhibitManager, IdleNotifier, IdleNotifierEvent, Seat};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_compositor, wl_seat, wl_surface};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols::ext::idle_notify::v1::client::{ext_idle_notification_v1, ext_idle_notifier_v1};
use wayland_protocols::wp::idle_inhibit::zv1::client::{zwp_idle_inhibit_manager_v1, zwp_idle_inhibitor_v1};

#[derive(Default)]
struct State {
    idled: usize,
    resumed: usize,
}

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_seat::WlSeat,
    zwp_idle_inhibit_manager_v1::ZwpIdleInhibitManagerV1,
    zwp_idle_inhibitor_v1::ZwpIdleInhibitorV1,
    ext_idle_notifier_v1::ExtIdleNotifierV1,
);

impl Dispatch<ext_idle_notification_v1::ExtIdleNotificationV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &ext_idle_notification_v1::ExtIdleNotificationV1,
        event: ext_idle_notification_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            ext_idle_notification_v1::Event::Idled => state.idled += 1,
            ext_idle_notification_v1::Event::Resumed => state.resumed += 1,
            _ => {}
        }
    }
}

fn inhibit_client(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let inhibits: zwp_idle_inhibit_manager_v1::ZwpIdleInhibitManagerV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let surface = compositor.create_surface(&qh, ());
    let inhibitor = inhibits.create_inhibitor(&surface, &qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("inhibiting");

    inhibitor.destroy();
    queue.roundtrip(&mut state).unwrap();
    step.step("released");
}

#[test]
fn inhibitors_count_while_visible() {
    let display = Display::new().expect("display");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let inhibits = IdleInhibitManager::new(&display).expect("idle-inhibit");

    let mut peer = Peer::spawn(&display, inhibit_client);
    peer.wait(&display, "inhibiting");
    let events = drain(|| inhibits.next_event());
    let [IdleInhibitEvent::NewInhibitor { inhibitor, surface }, IdleInhibitEvent::Changed(true)] = events[..] else {
        panic!("{events:?}");
    };
    assert_eq!(inhibits.inhibitor_surface(inhibitor), Some(surface));
    assert!(inhibits.is_visible(inhibitor), "visible until the compositor says otherwise");
    assert!(inhibits.is_inhibited());
    assert_eq!(inhibits.inhibitor_count(), 1);

    // Off screen: it no longer keeps anything awake.
    inhibits.set_visible(inhibitor, false);
    assert_eq!(drain(|| inhibits.next_event()), [IdleInhibitEvent::Changed(false)]);
    assert!(!inhibits.is_inhibited());
    inhibits.set_visible(inhibitor, true);
    assert_eq!(drain(|| inhibits.next_event()), [IdleInhibitEvent::Changed(true)]);
    peer.go();

    peer.wait(&display, "released");
    let events = drain(|| inhibits.next_event());
    assert!(events.contains(&IdleInhibitEvent::InhibitorDestroy(inhibitor)), "{events:?}");
    assert_eq!(events.last(), Some(&IdleInhibitEvent::Changed(false)));
    assert!(!inhibits.is_inhibited());
    assert_eq!(inhibits.inhibitor_count(), 0);
    assert_eq!(inhibits.inhibitor_surface(inhibitor), None);
    peer.finish(&display);
}

fn notify_client(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=7, ()).unwrap();
    let notifier: ext_idle_notifier_v1::ExtIdleNotifierV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let notification = notifier.get_idle_notification(20, &seat, &qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("watching");

    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.idled, 1);
    step.step("idled");

    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.resumed, 1);
    notification.destroy();
    queue.roundtrip(&mut state).unwrap();
    step.step("done");
}

#[test]
fn notifications_idle_and_resume() {
    let display = Display::new().expect("display");
    let _seat = Seat::new(&display, "seat0").expect("wl_seat");
    let notifier = IdleNotifier::new(&display).expect("idle-notify");

    let mut peer = Peer::spawn(&display, notify_client);
    peer.wait(&display, "watching");
    let events = drain(|| notifier.next_event());
    let [IdleNotifierEvent::NewNotification(n)] = events[..] else {
        panic!("{events:?}");
    };
    let info = notifier.notification(n).expect("notification");
    assert_eq!(info.timeout_ms, 20);
    assert!(!info.input_only);
    assert_eq!(notifier.notification_count(), 1);

    // Its timer runs on the display's loop.
    let mut seen = Vec::new();
    pump(&display, "the notification to idle", || {
        seen.extend(drain(|| notifier.next_event()));
        !seen.is_empty()
    });
    assert_eq!(seen, [IdleNotifierEvent::StateChange { notification: n, idled: true }]);
    assert!(notifier.notification(n).unwrap().idled);
    assert_eq!(notifier.idled_count(), 1);
    peer.go();

    peer.wait(&display, "idled");
    notifier.notify_activity();
    assert_eq!(
        drain(|| notifier.next_event()),
        [IdleNotifierEvent::StateChange { notification: n, idled: false }]
    );
    assert_eq!(notifier.idled_count(), 0);
    notifier.set_inhibited(true);
    assert!(notifier.is_inhibited());
    peer.go();

    peer.wait(&display, "done");
    assert_eq!(drain(|| notifier.next_event()), [IdleNotifierEvent::NotificationDestroy(n)]);
    assert_eq!(notifier.notification(n), None);
    assert_eq!(notifier.notification_count(), 0);
    peer.finish(&display);
}
