//! xdg-activation requests and selection changes, driven by a real client:
//! a token issued and spent (and a forged one), and a clipboard and a primary
//! selection set, read back through a pipe, and cleared.

#[macro_use]
mod common;

use std::io::{pipe, Read, Write};
use std::os::fd::OwnedFd;

use common::{ClientSide, Peer, take};
use luminaria::protocol::SelectionChange;
use luminaria::core::Display;
use luminaria::protocol::{Compositor, DataDeviceManager, PrimarySelectionManager, Seat, XdgActivation};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_compositor, wl_data_device, wl_data_device_manager, wl_data_offer, wl_data_source, wl_seat, wl_surface};
use wayland_client::{event_created_child, Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols::wp::primary_selection::zv1::client::{
    zwp_primary_selection_device_manager_v1, zwp_primary_selection_device_v1, zwp_primary_selection_offer_v1,
    zwp_primary_selection_source_v1,
};
use wayland_protocols::xdg::activation::v1::client::{xdg_activation_token_v1, xdg_activation_v1};

#[derive(Default)]
struct State {
    token: Option<String>,
    /// How many times a source was asked for its contents.
    sent: usize,
}

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_seat::WlSeat,
    xdg_activation_v1::XdgActivationV1,
    wl_data_device_manager::WlDataDeviceManager,
    wl_data_offer::WlDataOffer,
    zwp_primary_selection_device_manager_v1::ZwpPrimarySelectionDeviceManagerV1,
    zwp_primary_selection_offer_v1::ZwpPrimarySelectionOfferV1,
);

impl Dispatch<xdg_activation_token_v1::XdgActivationTokenV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &xdg_activation_token_v1::XdgActivationTokenV1,
        event: xdg_activation_token_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_activation_token_v1::Event::Done { token } = event {
            state.token = Some(token);
        }
    }
}

impl Dispatch<wl_data_device::WlDataDevice, ()> for State {
    fn event(_: &mut Self, _: &wl_data_device::WlDataDevice, _: wl_data_device::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}

    event_created_child!(State, wl_data_device::WlDataDevice, [
        wl_data_device::EVT_DATA_OFFER_OPCODE => (wl_data_offer::WlDataOffer, ()),
    ]);
}

impl Dispatch<zwp_primary_selection_device_v1::ZwpPrimarySelectionDeviceV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &zwp_primary_selection_device_v1::ZwpPrimarySelectionDeviceV1,
        _: zwp_primary_selection_device_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }

    event_created_child!(State, zwp_primary_selection_device_v1::ZwpPrimarySelectionDeviceV1, [
        zwp_primary_selection_device_v1::EVT_DATA_OFFER_OPCODE => (zwp_primary_selection_offer_v1::ZwpPrimarySelectionOfferV1, ()),
    ]);
}

/// What either kind of source answers a read with.
fn answer(state: &mut State, mime_type: &str, fd: OwnedFd, contents: &[u8]) {
    assert_eq!(mime_type, "text/plain");
    std::fs::File::from(fd).write_all(contents).expect("write selection");
    state.sent += 1;
}

impl Dispatch<wl_data_source::WlDataSource, ()> for State {
    fn event(state: &mut Self, _: &wl_data_source::WlDataSource, event: wl_data_source::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
        if let wl_data_source::Event::Send { mime_type, fd } = event {
            answer(state, &mime_type, fd, b"clipboard");
        }
    }
}

impl Dispatch<zwp_primary_selection_source_v1::ZwpPrimarySelectionSourceV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &zwp_primary_selection_source_v1::ZwpPrimarySelectionSourceV1,
        event: zwp_primary_selection_source_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let zwp_primary_selection_source_v1::Event::Send { mime_type, fd } = event {
            answer(state, &mime_type, fd, b"primary");
        }
    }
}

fn activation_client(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let activation: xdg_activation_v1::XdgActivationV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let launcher = compositor.create_surface(&qh, ());
    let window = compositor.create_surface(&qh, ());
    let token = activation.get_activation_token(&qh, ());
    token.set_surface(&launcher);
    token.set_app_id("org.luminaria.test".into());
    token.commit();
    queue.roundtrip(&mut state).unwrap();
    let issued = state.token.take().expect("no token issued");
    activation.activate(issued, &window);
    activation.activate("forged".into(), &launcher);
    queue.roundtrip(&mut state).unwrap();
    step.step("activated");
}

#[test]
fn activation_requests() {
    let display = Display::new().expect("display");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let _activation = XdgActivation::new(&display).expect("xdg-activation");

    let mut peer = Peer::spawn(&display, activation_client);
    peer.wait(&display, "activated");
    let events = take::<luminaria::protocol::ActivationEvent>();
    assert!(events.iter().any(|e| matches!(e, luminaria::protocol::ActivationEvent::TokenRequest(t) if t.app_id == "org.luminaria.test" && t.granted)));
    let requests: Vec<_> = events
        .into_iter()
        .filter_map(|e| match e {
            luminaria::protocol::ActivationEvent::Request(r) => Some(r),
            _ => None,
        })
        .collect();
    assert_eq!(requests.len(), 2, "{requests:?}");
    let (spent, forged) = (&requests[0], &requests[1]);
    assert!(spent.token_valid);
    assert_eq!(spent.app_id, "org.luminaria.test");
    let launcher = spent.requesting_surface.expect("the token named its surface");
    assert_ne!(spent.surface, launcher);
    assert!(!forged.token_valid);
    assert_eq!(forged.surface, launcher);
    assert_eq!(forged.app_id, "");
    peer.finish(&display);
}

fn selection_client(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=7, ()).unwrap();
    let devices: wl_data_device_manager::WlDataDeviceManager = globals.bind(&qh, 1..=3, ()).unwrap();
    let primaries: zwp_primary_selection_device_manager_v1::ZwpPrimarySelectionDeviceManagerV1 =
        globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let device = devices.get_data_device(&seat, &qh, ());
    let source = devices.create_data_source(&qh, ());
    source.offer("text/plain".into());
    device.set_selection(Some(&source), 0);
    let primary_device = primaries.get_device(&seat, &qh, ());
    let primary_source = primaries.create_source(&qh, ());
    primary_source.offer("text/plain".into());
    primary_device.set_selection(Some(&primary_source), 0);
    queue.roundtrip(&mut state).unwrap();
    step.step("selected");

    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.sent, 2, "a source was never asked for its contents");
    step.step("sent");

    device.set_selection(None, 0);
    primary_device.set_selection(None, 0);
    queue.roundtrip(&mut state).unwrap();
    assert!(source.is_alive());
    step.step("cleared");
}

#[test]
fn selection_changes_and_reads() {
    let display = Display::new().expect("display");
    let seat = Seat::new(&display, "seat0").expect("wl_seat");
    let clipboard = DataDeviceManager::new(&display, &seat).expect("data-device");
    let primary = PrimarySelectionManager::new(&display, &seat).expect("primary-selection");
    assert!(clipboard.selection_mime_types().is_empty());

    let mut peer = Peer::spawn(&display, selection_client);
    peer.wait(&display, "selected");
    assert_eq!(
        take::<SelectionChange>(),
        [
            SelectionChange { primary: false, mime_types: vec!["text/plain".to_string()] },
            SelectionChange { primary: true, mime_types: vec!["text/plain".to_string()] },
        ]
    );
    assert_eq!(clipboard.selection_mime_types(), ["text/plain"]);
    assert_eq!(primary.selection_mime_types(), ["text/plain"]);

    // Ask both owners for their contents; they write once dispatched.
    let (mut clip_read, clip_write) = pipe().expect("pipe");
    let (mut primary_read, primary_write) = pipe().expect("pipe");
    assert!(clipboard.selection_receive("text/plain", clip_write.into()));
    assert!(primary.selection_receive("text/plain", primary_write.into()));
    display.flush_clients();
    peer.go();
    peer.wait(&display, "sent");
    let mut text = String::new();
    clip_read.read_to_string(&mut text).expect("read clipboard");
    assert_eq!(text, "clipboard");
    text.clear();
    primary_read.read_to_string(&mut text).expect("read primary");
    assert_eq!(text, "primary");
    peer.go();

    peer.wait(&display, "cleared");
    assert_eq!(
        take::<SelectionChange>(),
        [
            SelectionChange { primary: false, mime_types: Vec::new() },
            SelectionChange { primary: true, mime_types: Vec::new() },
        ]
    );
    let (_, unused) = pipe().expect("pipe");
    assert!(!clipboard.selection_receive("text/plain", unused.into()), "nothing to read");
    peer.finish(&display);
}
