//! What a compositor tells a client about how it should draw: the integer
//! preferred buffer scale and transform, the fractional scale, and the surface
//! tree a pointer grab has to be placed in. All driven by a real client over a
//! socketpair, so the events are the ones a toolkit would see.

#[macro_use]
mod common;

use common::{dispatch, ClientSide, Peer, take};
use luminaria::core::Display;
use luminaria::protocol::{Compositor, CompositorEvent, FractionalScaleManager, Subcompositor, SurfaceId};
use luminaria::util::Transform;
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_compositor, wl_output, wl_subcompositor, wl_subsurface, wl_surface};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols::wp::fractional_scale::v1::client::{
    wp_fractional_scale_manager_v1, wp_fractional_scale_v1,
};

#[derive(Default)]
struct State {
    /// What `wp_fractional_scale_v1` last told the surface.
    fractional_scale: Option<u32>,
    preferred_scale: Option<i32>,
    preferred_transform: Option<wl_output::Transform>,
}

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_subcompositor::WlSubcompositor,
    wl_subsurface::WlSubsurface,
    wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1,
);

impl Dispatch<wl_surface::WlSurface, ()> for State {
    fn event(
        state: &mut Self,
        _: &wl_surface::WlSurface,
        event: wl_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_surface::Event::PreferredBufferScale { factor } => state.preferred_scale = Some(factor),
            wl_surface::Event::PreferredBufferTransform { transform } => {
                state.preferred_transform = transform.into_result().ok()
            }
            _ => {}
        }
    }
}

impl Dispatch<wp_fractional_scale_v1::WpFractionalScaleV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &wp_fractional_scale_v1::WpFractionalScaleV1,
        event: wp_fractional_scale_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wp_fractional_scale_v1::Event::PreferredScale { scale } = event {
            state.fractional_scale = Some(scale);
        }
    }
}

/// The first surface a client creates, as the server names it. The events are
/// queued by the time the peer reaches its first step; drain them there.
fn first_surface(_compositor: &Compositor<'_>, display: &Display) -> SurfaceId {
    for _ in 0..8 {
        for event in take::<CompositorEvent>() {
            if let CompositorEvent::NewSurface(surface) = event {
                return surface;
            }
        }
        dispatch(display);
    }
    panic!("the client created no surface");
}

fn client_scales(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    // v6 is where the preferred scale/transform events appear.
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let fractions: wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1 =
        globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let surface = compositor.create_surface(&qh, ());
    let _fractional = fractions.get_fractional_scale(&surface, &qh, ());
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("ready");

    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.preferred_scale, Some(2), "integer hint");
    assert_eq!(
        state.preferred_transform,
        Some(wl_output::Transform::_90),
        "preferred transform"
    );
    assert_eq!(state.fractional_scale, Some(180), "fractional scale in 120ths");
    step.step("scaled");

    // 2.0 is 240 in 120ths.
    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.fractional_scale, Some(240), "set_scale(2.0)");
    step.step("rescaled");
}

#[test]
fn preferred_scale_and_fractional_scale_reach_the_client() {
    let display = Display::new().expect("display");
    let compositor = Compositor::new(&display).expect("wl_compositor");
    let fractions = FractionalScaleManager::new(&display).expect("fractional-scale");

    let mut peer = Peer::spawn(&display, client_scales);
    peer.wait(&display, "ready");
    let surface = first_surface(&compositor, &display);

    surface.set_preferred_buffer_scale(2);
    surface.set_preferred_buffer_transform(Transform::Rotate90);
    fractions.set_scale(surface, 1.5); // and the integer hint, rounded up: 2
    peer.go();

    peer.wait(&display, "scaled");
    fractions.set_scale(surface, 2.0);
    peer.go();

    peer.wait(&display, "rescaled");
    peer.finish(&display);
}

fn client_tree(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let subcompositor: wl_subcompositor::WlSubcompositor = globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let parent = compositor.create_surface(&qh, ());
    let child = compositor.create_surface(&qh, ());
    let sub = subcompositor.get_subsurface(&child, &parent, &qh, ());
    sub.set_position(12, 34);
    parent.commit();
    child.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("subsurface");
    queue.roundtrip(&mut state).unwrap();
}

#[test]
fn surface_tree_reports_subsurface_offsets() {
    let display = Display::new().expect("display");
    let compositor = Compositor::new(&display).expect("wl_compositor");
    let _subcompositor = Subcompositor::new(&display).expect("wl_subcompositor");

    let mut peer = Peer::spawn(&display, client_tree);
    peer.wait(&display, "subsurface");

    let root = first_surface(&compositor, &display);
    let tree = root.surface_tree();
    assert!(tree.len() >= 2, "parent and child, got {tree:?}");
    assert_eq!((tree[0].surface, tree[0].x, tree[0].y), (root, 0, 0));
    assert_eq!((tree[1].x, tree[1].y), (12, 34), "subsurface offset: {tree:?}");
    assert_ne!(tree[1].surface, root);

    // A stale id finds nothing rather than faulting.
    assert!(SurfaceId(0).surface_tree().is_empty());

    peer.finish(&display);
}
