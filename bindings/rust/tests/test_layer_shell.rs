//! wlr-layer-shell events, driven by a real client: a panel from creation to
//! destruction through the LayerManager (layout, exclusive zone, scene,
//! keyboard), its popup, and the output a surface asked for.

#[macro_use]
mod common;

use common::{ClientSide, Peer, shm_buffer, take};
use luminaria::core::Display;
use luminaria::protocol::{
    Compositor, KeyboardInteractivity, Layer, LayerShell, LayerSurfaceEvent, Margins, OutputGlobal,
    PopupEvent, SurfaceId, XdgEvent, XdgShell,
};
use luminaria::shell::{LayerManager, PopupTree, SceneItem};
use luminaria::util::{Rect, edge};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_output, wl_shm, wl_shm_pool, wl_surface,
};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols::xdg::shell::client::{xdg_popup, xdg_positioner, xdg_surface, xdg_wm_base};
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};

#[derive(Default)]
struct State {
    /// (width, height) of every layer-surface configure.
    configures: Vec<(u32, u32)>,
    closed: bool,
}

registry_state!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_shm::WlShm,
    wl_shm_pool::WlShmPool,
    wl_buffer::WlBuffer,
    wl_output::WlOutput,
    xdg_positioner::XdgPositioner,
    xdg_popup::XdgPopup,
    zwlr_layer_shell_v1::ZwlrLayerShellV1,
);

impl Dispatch<xdg_wm_base::XdgWmBase, ()> for State {
    fn event(
        _: &mut Self,
        wm: &xdg_wm_base::XdgWmBase,
        event: xdg_wm_base::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            wm.pong(serial);
        }
    }
}

impl Dispatch<xdg_surface::XdgSurface, ()> for State {
    fn event(
        _: &mut Self,
        xs: &xdg_surface::XdgSurface,
        event: xdg_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            xs.ack_configure(serial);
        }
    }
}

impl Dispatch<zwlr_layer_surface_v1::ZwlrLayerSurfaceV1, ()> for State {
    fn event(
        state: &mut Self,
        ls: &zwlr_layer_surface_v1::ZwlrLayerSurfaceV1,
        event: zwlr_layer_surface_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_layer_surface_v1::Event::Configure {
                serial,
                width,
                height,
            } => {
                ls.ack_configure(serial);
                state.configures.push((width, height));
            }
            zwlr_layer_surface_v1::Event::Closed => state.closed = true,
            _ => {}
        }
    }
}

fn client(conn: Connection, step: ClientSide) {
    use zwlr_layer_surface_v1::Anchor;
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let shm: wl_shm::WlShm = globals.bind(&qh, 1..=1, ()).unwrap();
    let output: wl_output::WlOutput = globals.bind(&qh, 1..=4, ()).unwrap();
    let wm: xdg_wm_base::XdgWmBase = globals.bind(&qh, 1..=6, ()).unwrap();
    let layers: zwlr_layer_shell_v1::ZwlrLayerShellV1 = globals.bind(&qh, 1..=4, ()).unwrap();
    let mut state = State::default();

    // A panel along the top, for whichever output the compositor picks.
    let surface = compositor.create_surface(&qh, ());
    let panel = layers.get_layer_surface(
        &surface,
        None,
        zwlr_layer_shell_v1::Layer::Top,
        "panel".into(),
        &qh,
        (),
    );
    panel.set_size(0, 30);
    panel.set_anchor(Anchor::Top | Anchor::Left | Anchor::Right);
    panel.set_exclusive_zone(30);
    panel.set_margin(5, 0, 0, 0);
    panel.set_keyboard_interactivity(zwlr_layer_surface_v1::KeyboardInteractivity::OnDemand);
    surface.commit();
    // A wallpaper on the output the client bound.
    let wall_surface = compositor.create_surface(&qh, ());
    let wall = layers.get_layer_surface(
        &wall_surface,
        Some(&output),
        zwlr_layer_shell_v1::Layer::Background,
        "wallpaper".into(),
        &qh,
        (),
    );
    wall.set_anchor(Anchor::Top | Anchor::Bottom | Anchor::Left | Anchor::Right);
    wall.set_exclusive_zone(-1);
    wall_surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("committed");

    // The compositor laid the panel out: the output's width, its own height.
    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.configures.last(), Some(&(1000, 30)));
    let buffer = shm_buffer(&shm, &qh, 1000, 30);
    surface.attach(Some(&buffer), 0, 0);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("mapped");

    // The panel's menu: an xdg popup with no xdg parent, claimed by the panel.
    let positioner = wm.create_positioner(&qh, ());
    positioner.set_size(80, 120);
    positioner.set_anchor_rect(0, 30, 1, 1);
    let popup_surface = compositor.create_surface(&qh, ());
    let popup_xdg = wm.get_xdg_surface(&popup_surface, &qh, ());
    let popup = popup_xdg.get_popup(None, &positioner, &qh, ());
    panel.get_popup(&popup);
    popup_surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("popup");

    queue.roundtrip(&mut state).unwrap();
    assert!(state.closed, "closed never arrived");
    popup.destroy();
    popup_xdg.destroy();
    panel.destroy();
    queue.roundtrip(&mut state).unwrap();
    step.step("destroyed");
}

const OUTPUT: u32 = 1;

#[test]
fn layer_surfaces_through_the_layer_manager() {
    let display = Display::new().expect("display");
    display.init_shm().expect("wl_shm");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let shell = XdgShell::new(&display).expect("xdg-shell");
    let layers = LayerShell::new(&display).expect("layer-shell");
    let output = OutputGlobal::new(&display, 1000, 800, "test-1").expect("wl_output");
    let mut manager = LayerManager::new().expect("layer manager");
    let popups = PopupTree::new(&shell).expect("popup tree");
    let full = Rect::new(0, 0, 1000, 800);

    let mut peer = Peer::spawn(&display, client);
    peer.wait(&display, "committed");
    let events = take::<(SurfaceId, LayerSurfaceEvent)>();
    let (panel, wall) = match events[..] {
        [
            (p, LayerSurfaceEvent::Created),
            _,
            (w, LayerSurfaceEvent::Created),
            _,
        ] => (p, w),
        _ => panic!("{events:?}"),
    };
    assert_eq!(
        events,
        [
            (panel, LayerSurfaceEvent::Created),
            (panel, LayerSurfaceEvent::StateChange),
            (wall, LayerSurfaceEvent::Created),
            (wall, LayerSurfaceEvent::StateChange),
        ]
    );
    let ls = layers.layer_surface(panel).expect("panel");
    assert_eq!(ls.scope(), "panel");
    assert_eq!(ls.layer(), Layer::Top);
    assert_eq!(ls.anchor(), edge::TOP | edge::LEFT | edge::RIGHT);
    assert_eq!(ls.desired_size(), (0, 30));
    assert_eq!(ls.exclusive_zone(), 30);
    assert_eq!(ls.exclusive_edge(), 0);
    assert_eq!(
        ls.margins(),
        Margins {
            top: 5,
            ..Margins::default()
        }
    );
    assert_eq!(ls.keyboard_interactivity(), KeyboardInteractivity::OnDemand);
    assert!(!ls.is_mapped());
    assert!(!ls.has_output() && !ls.is_on(&output));
    let wallpaper = layers.layer_surface(wall).expect("wallpaper");
    assert_eq!(wallpaper.layer(), Layer::Background);
    assert!(wallpaper.has_output() && wallpaper.is_on(&output));
    let mut listed = layers.layer_surfaces();
    listed.sort();
    let mut expected = vec![panel, wall];
    expected.sort();
    assert_eq!(listed, expected);

    // Not mapped yet: laid out and configured, but taking no room.
    assert!(manager.add(&layers, panel, OUTPUT));
    assert_eq!(manager.arrange(OUTPUT, full, full), full);
    assert_eq!(ls.pending_size(), (1000, 30));
    peer.go();

    peer.wait(&display, "mapped");
    assert_eq!(
        take::<(SurfaceId, LayerSurfaceEvent)>(),
        [(panel, LayerSurfaceEvent::Map)]
    );
    assert!(ls.is_mapped());
    manager.set_mapped(panel, true);
    let usable = manager.arrange(OUTPUT, full, full);
    assert_eq!(
        usable,
        Rect::new(0, 35, 1000, 765),
        "zone plus margin come off the top"
    );
    // The same layout, one surface by hand.
    let mut by_hand = full;
    assert_eq!(ls.arrange(full, &mut by_hand), Rect::new(0, 5, 1000, 30));
    assert_eq!(by_hand, usable);

    let mut scene: Vec<SceneItem> = Vec::new();
    manager.append_to(&mut scene, OUTPUT, Layer::Bottom);
    assert!(scene.is_empty());
    manager.append_to(&mut scene, OUTPUT, Layer::Top);
    assert_eq!(scene.len(), 1);
    assert_eq!(scene[0].surface, panel);
    assert_eq!(scene[0].rect, Rect::new(0, 5, 1000, 30));
    assert_eq!(manager.keyboard_target(panel), Some(panel));
    peer.go();

    peer.wait(&display, "popup");
    let events = take::<(SurfaceId, LayerSurfaceEvent)>();
    let [(_, LayerSurfaceEvent::NewPopup(popup))] = events[..] else {
        panic!("{events:?}");
    };
    assert_eq!(events, [(panel, LayerSurfaceEvent::NewPopup(popup))]);
    assert_eq!(
        take::<XdgEvent>(),
        [XdgEvent::Popup(popup, PopupEvent::Created { parent: None })]
    );
    assert_eq!(shell.popup(popup).expect("popup").parent(), None);
    // Positioned against where the panel was put.
    assert!(popups.set_parent(&shell, popup, panel));

    ls.close();
    common::collect(&display);
    assert_eq!(
        take::<(SurfaceId, LayerSurfaceEvent)>(),
        [(panel, LayerSurfaceEvent::Unmap)]
    );
    peer.go();

    peer.wait(&display, "destroyed");
    let events = take::<(SurfaceId, LayerSurfaceEvent)>();
    assert!(
        events.contains(&(panel, LayerSurfaceEvent::Destroy)),
        "{events:?}"
    );
    // The manager let go of it by itself.
    assert_eq!(manager.keyboard_target(panel), None);
    let mut scene: Vec<SceneItem> = Vec::new();
    manager.append_to(&mut scene, OUTPUT, Layer::Top);
    assert!(scene.is_empty());
    assert!(layers.layer_surface(panel).is_none());
    assert_eq!(ls.scope(), "");
    peer.finish(&display);
    assert!(take::<(SurfaceId, LayerSurfaceEvent)>().contains(&(wall, LayerSurfaceEvent::Destroy)));
}
