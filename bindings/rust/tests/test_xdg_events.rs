//! xdg-shell events, driven by a real client: a toplevel's lifecycle, every
//! request it can make and the state the compositor answers with, and popups
//! from creation through the PopupTree into the scene and out again.

#[macro_use]
mod common;

use common::{shm_buffer, ClientSide, Peer, take};
use luminaria::core::Display;
use luminaria::protocol::{
    Compositor, PopupArea, PopupEvent, Seat, SurfaceId, ToplevelEvent, ToplevelStates, XdgEvent, XdgShell,
};
use luminaria::shell::{scene_hit_test, PopupTree, SceneItem};
use luminaria::util::{edge, Rect};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_buffer, wl_compositor, wl_seat, wl_shm, wl_shm_pool, wl_surface};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols::xdg::shell::client::{xdg_popup, xdg_positioner, xdg_surface, xdg_toplevel, xdg_wm_base};

#[derive(Default)]
struct State {
    /// (width, height, states) of every xdg_toplevel.configure.
    toplevel_configures: Vec<(i32, i32, Vec<u32>)>,
    closed: bool,
    /// (x, y, width, height) of every xdg_popup.configure.
    popup_configures: Vec<(i32, i32, i32, i32)>,
    repositioned: Option<u32>,
    popup_done: bool,
}

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_shm::WlShm,
    wl_shm_pool::WlShmPool,
    wl_buffer::WlBuffer,
    wl_seat::WlSeat,
    xdg_positioner::XdgPositioner,
);

impl Dispatch<xdg_wm_base::XdgWmBase, ()> for State {
    fn event(_: &mut Self, wm: &xdg_wm_base::XdgWmBase, event: xdg_wm_base::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            wm.pong(serial);
        }
    }
}

impl Dispatch<xdg_surface::XdgSurface, ()> for State {
    fn event(_: &mut Self, xs: &xdg_surface::XdgSurface, event: xdg_surface::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
        if let xdg_surface::Event::Configure { serial } = event {
            xs.ack_configure(serial);
        }
    }
}

impl Dispatch<xdg_toplevel::XdgToplevel, ()> for State {
    fn event(state: &mut Self, _: &xdg_toplevel::XdgToplevel, event: xdg_toplevel::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
        match event {
            xdg_toplevel::Event::Configure { width, height, states } => {
                let states = states
                    .chunks_exact(4)
                    .map(|b| u32::from_ne_bytes([b[0], b[1], b[2], b[3]]))
                    .collect();
                state.toplevel_configures.push((width, height, states));
            }
            xdg_toplevel::Event::Close => state.closed = true,
            _ => {}
        }
    }
}

impl Dispatch<xdg_popup::XdgPopup, ()> for State {
    fn event(state: &mut Self, _: &xdg_popup::XdgPopup, event: xdg_popup::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
        match event {
            xdg_popup::Event::Configure { x, y, width, height } => state.popup_configures.push((x, y, width, height)),
            xdg_popup::Event::Repositioned { token } => state.repositioned = Some(token),
            xdg_popup::Event::PopupDone => state.popup_done = true,
            _ => {}
        }
    }
}

/// The globals every script binds.
struct Globals {
    compositor: wl_compositor::WlCompositor,
    shm: wl_shm::WlShm,
    wm: xdg_wm_base::XdgWmBase,
    seat: wl_seat::WlSeat,
}

fn bind(conn: &Connection) -> (Globals, wayland_client::EventQueue<State>, QueueHandle<State>) {
    let (globals, queue) = registry_queue_init::<State>(conn).expect("registry");
    let qh = queue.handle();
    let g = Globals {
        compositor: globals.bind(&qh, 1..=6, ()).expect("wl_compositor"),
        shm: globals.bind(&qh, 1..=1, ()).expect("wl_shm"),
        wm: globals.bind(&qh, 1..=6, ()).expect("xdg_wm_base"),
        seat: globals.bind(&qh, 1..=7, ()).expect("wl_seat"),
    };
    (g, queue, qh)
}

const MAXIMIZED: u32 = 1;
const ACTIVATED: u32 = 4;

fn toplevel_client(conn: Connection, step: ClientSide) {
    let (g, mut queue, qh) = bind(&conn);
    let mut state = State::default();

    let surface = g.compositor.create_surface(&qh, ());
    let xs = g.wm.get_xdg_surface(&surface, &qh, ());
    let top = xs.get_toplevel(&qh, ());
    top.set_title("first".into());
    top.set_app_id("org.luminaria.test".into());
    top.set_min_size(100, 50);
    top.set_max_size(800, 600);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.toplevel_configures.len(), 1, "no initial configure");
    let buffer = shm_buffer(&g.shm, &qh, 64, 64);
    surface.attach(Some(&buffer), 0, 0);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("mapped");

    top.set_maximized();
    top.unset_maximized();
    top.set_fullscreen(None);
    top.unset_fullscreen();
    top.set_minimized();
    top._move(&g.seat, 7);
    top.resize(&g.seat, 8, xdg_toplevel::ResizeEdge::BottomRight);
    top.show_window_menu(&g.seat, 9, 10, 20);
    let surface2 = g.compositor.create_surface(&qh, ());
    let xs2 = g.wm.get_xdg_surface(&surface2, &qh, ());
    let dialog = xs2.get_toplevel(&qh, ());
    dialog.set_parent(Some(&top));
    dialog.set_parent(None);
    queue.roundtrip(&mut state).unwrap();
    // Nobody but the compositor answers a request, and it has not yet.
    assert!(state.toplevel_configures.iter().all(|(_, _, s)| !s.contains(&MAXIMIZED)));
    step.step("requests");

    queue.roundtrip(&mut state).unwrap();
    let (w, h, states) = state.toplevel_configures.last().unwrap().clone();
    assert_eq!((w, h), (300, 200));
    assert!(states.contains(&MAXIMIZED) && states.contains(&ACTIVATED), "{states:?}");
    step.step("configured");

    queue.roundtrip(&mut state).unwrap();
    assert!(state.closed, "close never arrived");
    step.step("closed");
}

#[test]
fn toplevel_events_requests_and_state() {
    let display = Display::new().expect("display");
    display.init_shm().expect("wl_shm");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    // Answering maximize/fullscreen/minimize ourselves, so they arrive as events.
    let shell = XdgShell::new(&display).expect("xdg-shell").forward_window_requests();
    let _seat = Seat::new(&display, "seat0").expect("wl_seat");

    let mut peer = Peer::spawn(&display, toplevel_client);
    peer.wait(&display, "mapped");
    let events = take::<XdgEvent>();
    let XdgEvent::Toplevel(a, ToplevelEvent::Created) = events[0] else {
        panic!("{events:?}");
    };
    assert_eq!(
        events,
        [
            XdgEvent::Toplevel(a, ToplevelEvent::Created),
            XdgEvent::Toplevel(a, ToplevelEvent::IdentityChange),
            XdgEvent::Toplevel(a, ToplevelEvent::IdentityChange),
            XdgEvent::Toplevel(a, ToplevelEvent::Map),
        ]
    );
    let t = shell.toplevel(a).expect("toplevel");
    assert_eq!(t.title(), "first");
    assert_eq!(t.app_id(), "org.luminaria.test");
    assert_eq!(t.min_size(), (100, 50));
    assert_eq!(t.max_size(), (800, 600));
    assert!(t.is_mapped());
    assert_eq!(t.parent(), None);
    assert_eq!(t.geometry(), Rect::new(0, 0, 64, 64));
    assert_eq!(shell.toplevels(), [a]);
    peer.go();

    peer.wait(&display, "requests");
    let events = take::<XdgEvent>();
    let XdgEvent::Toplevel(b, ToplevelEvent::Created) = events[8] else {
        panic!("{events:?}");
    };
    assert_eq!(
        events,
        [
            XdgEvent::Toplevel(a, ToplevelEvent::RequestMaximize(true)),
            XdgEvent::Toplevel(a, ToplevelEvent::RequestMaximize(false)),
            XdgEvent::Toplevel(a, ToplevelEvent::RequestFullscreen(true)),
            XdgEvent::Toplevel(a, ToplevelEvent::RequestFullscreen(false)),
            XdgEvent::Toplevel(a, ToplevelEvent::RequestMinimize),
            XdgEvent::Toplevel(a, ToplevelEvent::RequestMove { serial: 7 }),
            XdgEvent::Toplevel(a, ToplevelEvent::RequestResize { serial: 8, edges: edge::BOTTOM | edge::RIGHT }),
            XdgEvent::Toplevel(a, ToplevelEvent::RequestWindowMenu { serial: 9, x: 10, y: 20 }),
            XdgEvent::Toplevel(b, ToplevelEvent::Created),
            XdgEvent::Toplevel(b, ToplevelEvent::ParentChange(Some(a))),
            XdgEvent::Toplevel(b, ToplevelEvent::ParentChange(None)),
        ]
    );
    // Forwarded, so luminaria's "nobody is arbitrating" fallback stays out of
    // it: the states are still ours.
    assert_eq!(t.states(), ToplevelStates::default());

    t.set_maximized(true);
    t.set_activated(true);
    t.set_activated(true); // no change, no event
    assert_ne!(t.configure(300, 200), 0);
    common::collect(&display);
    assert_eq!(
        take::<XdgEvent>(),
        [XdgEvent::Toplevel(a, ToplevelEvent::StateChange), XdgEvent::Toplevel(a, ToplevelEvent::StateChange)]
    );
    let states = t.states();
    assert!(states.maximized && states.activated && !states.fullscreen);
    assert_eq!(t.pending_size(), (300, 200));
    peer.go();

    peer.wait(&display, "configured");
    t.close();
    peer.go();
    peer.wait(&display, "closed");
    peer.finish(&display);

    // The client is gone, and both windows with it.
    let destroyed: Vec<SurfaceId> = take::<XdgEvent>()
        .into_iter()
        .filter_map(|e| match e {
            XdgEvent::Toplevel(s, ToplevelEvent::Destroy) => Some(s),
            _ => None,
        })
        .collect();
    assert!(destroyed.contains(&a) && destroyed.contains(&b), "{destroyed:?}");
    assert!(shell.toplevel(a).is_none());
    assert!(shell.toplevels().is_empty());
    assert_eq!(t.title(), "", "a stale handle reads nothing");
}

fn popup_client(conn: Connection, step: ClientSide) {
    let (g, mut queue, qh) = bind(&conn);
    let mut state = State::default();

    let parent_surface = g.compositor.create_surface(&qh, ());
    let parent = g.wm.get_xdg_surface(&parent_surface, &qh, ());
    let _top = parent.get_toplevel(&qh, ());
    parent_surface.commit();
    queue.roundtrip(&mut state).unwrap();
    let parent_buffer = shm_buffer(&g.shm, &qh, 200, 100);
    parent_surface.attach(Some(&parent_buffer), 0, 0);
    parent_surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("parent mapped");

    let positioner = g.wm.create_positioner(&qh, ());
    positioner.set_size(50, 40);
    positioner.set_anchor_rect(10, 10, 1, 1);
    positioner.set_anchor(xdg_positioner::Anchor::TopLeft);
    positioner.set_gravity(xdg_positioner::Gravity::BottomRight);
    let surface = g.compositor.create_surface(&qh, ());
    let xs = g.wm.get_xdg_surface(&surface, &qh, ());
    let popup = xs.get_popup(Some(&parent), &positioner, &qh, ());
    popup.grab(&g.seat, 42);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.popup_configures.last(), Some(&(10, 10, 50, 40)));
    let buffer = shm_buffer(&g.shm, &qh, 50, 40);
    surface.attach(Some(&buffer), 0, 0);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("popup mapped");

    // The same placement, now allowed to slide: the compositor said the
    // parent's screen ends at x = 140, and the popup would reach 160.
    let sliding = g.wm.create_positioner(&qh, ());
    sliding.set_size(50, 40);
    sliding.set_anchor_rect(10, 10, 1, 1);
    sliding.set_anchor(xdg_positioner::Anchor::TopLeft);
    sliding.set_gravity(xdg_positioner::Gravity::BottomRight);
    sliding.set_constraint_adjustment(xdg_positioner::ConstraintAdjustment::SlideX);
    popup.reposition(&sliding, 5);
    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.repositioned, Some(5));
    assert_eq!(state.popup_configures.last(), Some(&(-10, 10, 50, 40)));
    step.step("repositioned");

    queue.roundtrip(&mut state).unwrap();
    assert!(state.popup_done, "dismissed popup never told");
    popup.destroy();
    xs.destroy();
    queue.roundtrip(&mut state).unwrap();
    step.step("popup destroyed");
}

#[test]
fn popups_through_the_tree_into_the_scene() {
    let display = Display::new().expect("display");
    display.init_shm().expect("wl_shm");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let shell = XdgShell::new(&display).expect("xdg-shell");
    let _seat = Seat::new(&display, "seat0").expect("wl_seat");
    // Following the shell from before the first popup exists.
    let mut tree = PopupTree::new(&shell).expect("popup tree");

    let mut peer = Peer::spawn(&display, popup_client);
    peer.wait(&display, "parent mapped");
    let parent = match take::<XdgEvent>().first() {
        Some(&XdgEvent::Toplevel(s, ToplevelEvent::Created)) => s,
        other => panic!("{other:?}"),
    };
    peer.go();

    peer.wait(&display, "popup mapped");
    let events = take::<XdgEvent>();
    let XdgEvent::Popup(p, PopupEvent::Created { parent: Some(created_on) }) = events[0] else {
        panic!("{events:?}");
    };
    assert_eq!(created_on, parent);
    assert_eq!(
        events,
        [
            XdgEvent::Popup(p, PopupEvent::Created { parent: Some(parent) }),
            XdgEvent::Popup(p, PopupEvent::Grab { serial: 42 }),
            XdgEvent::Popup(p, PopupEvent::Map),
        ]
    );
    let popup = shell.popup(p).expect("popup");
    assert_eq!(popup.parent(), Some(parent));
    assert_eq!(popup.geometry(), Rect::new(10, 10, 50, 40));
    assert!(popup.is_mapped() && popup.has_grab());
    assert!(shell.toplevel(p).is_none(), "a popup is not a toplevel");

    // Already in the tree, mapped, and from there into the scene above its parent.
    let window = SceneItem::surface(parent, Rect::new(100, 50, 200, 100), 100, 50).with_tag(7);
    let mut scene = vec![window];
    tree.append_to(&mut scene);
    assert_eq!(scene.len(), 2);
    assert_eq!(scene[1].surface, p);
    assert_eq!(scene[1].rect, Rect::new(110, 60, 50, 40));
    assert_eq!(scene[1].surface_origin, (110, 60));
    assert_eq!(scene[1].tag, 7, "a popup carries its window's tag");
    let hit = scene_hit_test(&scene, 115.0, 65.0);
    assert_eq!(hit.surface, Some(p));
    assert!(tree.contains(p));
    assert_eq!(tree.active_grab(), Some(p));

    shell.set_popup_constraint_areas(&[PopupArea {
        parent,
        parent_box: Rect::new(100, 50, 200, 100),
        usable: Rect::new(0, 0, 140, 1000),
    }]);
    peer.go();

    peer.wait(&display, "repositioned");
    assert_eq!(take::<XdgEvent>(), [XdgEvent::Popup(p, PopupEvent::Reposition)]);
    assert_eq!(popup.geometry(), Rect::new(-10, 10, 50, 40));

    // A click on the parent is outside the popup: the grab goes.
    assert!(tree.dismiss_grabs_outside(Some(parent)));
    common::collect(&display);
    assert_eq!(take::<XdgEvent>(), [XdgEvent::Popup(p, PopupEvent::Unmap)]);
    assert!(!popup.is_mapped());
    // Unmapped: out of the scene, without anyone telling the tree.
    assert!(!tree.contains(p));
    peer.go();

    peer.wait(&display, "popup destroyed");
    assert_eq!(take::<XdgEvent>(), [XdgEvent::Popup(p, PopupEvent::Destroy)]);
    // The tree let go of it by itself, before Rust heard of the destroy.
    assert!(!tree.contains(p));
    let mut scene = vec![window];
    tree.append_to(&mut scene);
    assert_eq!(scene.len(), 1);
    assert!(shell.popup(p).is_none());
    peer.finish(&display);
}
