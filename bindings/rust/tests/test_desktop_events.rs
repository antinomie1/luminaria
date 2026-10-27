//! Tests for desktop protocol signals: DecorationRequest, ForeignToplevelRequest,
//! WorkspaceRequest, OutputConfigRequest, ToplevelIconChange, CursorShapeRequest,
//! and ActivationTokenRequest.

#[macro_use]
mod common;

use common::{ClientSide, Peer, take};
use luminaria::core::Display;
use luminaria::desktop::{
    ForeignToplevelManager, ForeignToplevelRequestKind, OutputManager, WorkspaceManager,
    WorkspaceRequestKind,
};
use luminaria::gpu::{Cursor, SceneRenderer, VulkanOptions};
use luminaria::protocol::{
    ActivationEvent, Compositor, CursorShapeManager, DecorationMode, Seat, XdgActivation,
    XdgDecorationManager, XdgShell, XdgToplevelIconManager,
};
use luminaria::shell::OutputLayout;
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_keyboard, wl_pointer, wl_seat, wl_shm, wl_shm_pool, wl_surface,
};
use wayland_client::{Connection, Dispatch, QueueHandle, event_created_child};
use wayland_protocols::ext::workspace::v1::client::{
    ext_workspace_group_handle_v1, ext_workspace_handle_v1, ext_workspace_manager_v1,
};
use wayland_protocols::wp::cursor_shape::v1::client::{
    wp_cursor_shape_device_v1, wp_cursor_shape_manager_v1,
};
use wayland_protocols::xdg::activation::v1::client::{xdg_activation_token_v1, xdg_activation_v1};
use wayland_protocols::xdg::decoration::zv1::client::{
    zxdg_decoration_manager_v1, zxdg_toplevel_decoration_v1,
};
use wayland_protocols::xdg::shell::client::xdg_wm_base;
use wayland_protocols::xdg::toplevel_icon::v1::client::{
    xdg_toplevel_icon_manager_v1, xdg_toplevel_icon_v1,
};
use wayland_protocols_wlr::foreign_toplevel::v1::client::{
    zwlr_foreign_toplevel_handle_v1, zwlr_foreign_toplevel_manager_v1,
};
use wayland_protocols_wlr::output_management::v1::client::{
    zwlr_output_configuration_v1, zwlr_output_head_v1, zwlr_output_manager_v1, zwlr_output_mode_v1,
};

#[derive(Default)]
struct State {
    pointer_enter_serial: Option<u32>,
    foreign_handle: Option<zwlr_foreign_toplevel_handle_v1::ZwlrForeignToplevelHandleV1>,
    group_handle: Option<ext_workspace_group_handle_v1::ExtWorkspaceGroupHandleV1>,
    #[allow(dead_code)]
    workspace_handle: Option<ext_workspace_handle_v1::ExtWorkspaceHandleV1>,
    output_manager_serial: Option<u32>,
}

registry_state!(State);
xdg_basics!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_seat::WlSeat,
    wl_shm::WlShm,
    wl_shm_pool::WlShmPool,
    wl_buffer::WlBuffer,
    wl_keyboard::WlKeyboard,
    zxdg_decoration_manager_v1::ZxdgDecorationManagerV1,
    zxdg_toplevel_decoration_v1::ZxdgToplevelDecorationV1,
    xdg_toplevel_icon_manager_v1::XdgToplevelIconManagerV1,
    xdg_toplevel_icon_v1::XdgToplevelIconV1,
    wp_cursor_shape_manager_v1::WpCursorShapeManagerV1,
    wp_cursor_shape_device_v1::WpCursorShapeDeviceV1,
    xdg_activation_v1::XdgActivationV1,
    xdg_activation_token_v1::XdgActivationTokenV1,
    ext_workspace_handle_v1::ExtWorkspaceHandleV1,
    zwlr_output_configuration_v1::ZwlrOutputConfigurationV1,
    zwlr_output_mode_v1::ZwlrOutputModeV1,
);

impl Dispatch<wl_pointer::WlPointer, ()> for State {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_pointer::Event::Enter { serial, .. } = event {
            state.pointer_enter_serial = Some(serial);
        }
    }
}

impl Dispatch<zwlr_foreign_toplevel_handle_v1::ZwlrForeignToplevelHandleV1, ()> for State {
    fn event(
        state: &mut Self,
        handle: &zwlr_foreign_toplevel_handle_v1::ZwlrForeignToplevelHandleV1,
        _: zwlr_foreign_toplevel_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        state.foreign_handle = Some(handle.clone());
    }
}

impl Dispatch<ext_workspace_group_handle_v1::ExtWorkspaceGroupHandleV1, ()> for State {
    fn event(
        state: &mut Self,
        handle: &ext_workspace_group_handle_v1::ExtWorkspaceGroupHandleV1,
        _: ext_workspace_group_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        state.group_handle = Some(handle.clone());
    }
}

impl Dispatch<ext_workspace_manager_v1::ExtWorkspaceManagerV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &ext_workspace_manager_v1::ExtWorkspaceManagerV1,
        _: ext_workspace_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }

    event_created_child!(State, ext_workspace_manager_v1::ExtWorkspaceManagerV1, [
        ext_workspace_manager_v1::EVT_WORKSPACE_GROUP_OPCODE => (ext_workspace_group_handle_v1::ExtWorkspaceGroupHandleV1, ()),
        ext_workspace_manager_v1::EVT_WORKSPACE_OPCODE => (ext_workspace_handle_v1::ExtWorkspaceHandleV1, ()),
    ]);
}

impl Dispatch<zwlr_foreign_toplevel_manager_v1::ZwlrForeignToplevelManagerV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &zwlr_foreign_toplevel_manager_v1::ZwlrForeignToplevelManagerV1,
        _: zwlr_foreign_toplevel_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }

    event_created_child!(State, zwlr_foreign_toplevel_manager_v1::ZwlrForeignToplevelManagerV1, [
        zwlr_foreign_toplevel_manager_v1::EVT_TOPLEVEL_OPCODE => (zwlr_foreign_toplevel_handle_v1::ZwlrForeignToplevelHandleV1, ()),
    ]);
}

impl Dispatch<zwlr_output_manager_v1::ZwlrOutputManagerV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &zwlr_output_manager_v1::ZwlrOutputManagerV1,
        event: zwlr_output_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let zwlr_output_manager_v1::Event::Done { serial } = event {
            state.output_manager_serial = Some(serial);
        }
    }

    event_created_child!(State, zwlr_output_manager_v1::ZwlrOutputManagerV1, [
        zwlr_output_manager_v1::EVT_HEAD_OPCODE => (zwlr_output_head_v1::ZwlrOutputHeadV1, ()),
    ]);
}

impl Dispatch<zwlr_output_head_v1::ZwlrOutputHeadV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &zwlr_output_head_v1::ZwlrOutputHeadV1,
        _: zwlr_output_head_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }

    event_created_child!(State, zwlr_output_head_v1::ZwlrOutputHeadV1, [
        zwlr_output_head_v1::EVT_MODE_OPCODE => (zwlr_output_mode_v1::ZwlrOutputModeV1, ()),
    ]);
}

// ----------------------------------------------------------------------------
// 1. Decoration and Toplevel Icon events
// ----------------------------------------------------------------------------

fn client_decoration_and_icon(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let wm: xdg_wm_base::XdgWmBase = globals.bind(&qh, 1..=6, ()).unwrap();
    let deco_mgr: zxdg_decoration_manager_v1::ZxdgDecorationManagerV1 =
        globals.bind(&qh, 1..=1, ()).unwrap();
    let icon_mgr: xdg_toplevel_icon_manager_v1::XdgToplevelIconManagerV1 =
        globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let surface = compositor.create_surface(&qh, ());
    let xsurf = wm.get_xdg_surface(&surface, &qh, ());
    let toplevel = xsurf.get_toplevel(&qh, ());

    // Request decoration
    let deco = deco_mgr.get_toplevel_decoration(&toplevel, &qh, ());
    deco.set_mode(zxdg_toplevel_decoration_v1::Mode::ServerSide);

    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("deco_requested");

    // Set toplevel icon
    let icon = icon_mgr.create_icon(&qh, ());
    icon.set_name("my-test-app".into());
    icon_mgr.set_icon(&toplevel, Some(&icon));
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("icon_set");
}

#[test]
fn test_decoration_and_icon_events() {
    let display = Display::new().expect("display");
    let _comp = Compositor::new(&display).expect("compositor");
    let _shell = XdgShell::new(&display).expect("shell");
    let _deco_mgr = XdgDecorationManager::new(&display).expect("decoration");
    let icon_mgr = XdgToplevelIconManager::new(&display).expect("icon");

    let mut peer = Peer::spawn(&display, client_decoration_and_icon);

    peer.wait(&display, "deco_requested");
    let events = take::<luminaria::protocol::DecorationRequest>();
    assert!(!events.is_empty(), "expected DecorationRequest: {events:?}");
    let req = events
        .iter()
        .find(|r| r.preferred == Some(DecorationMode::ServerSide))
        .expect("expected request with ServerSide preferred");
    assert_eq!(req.mode, DecorationMode::ServerSide);
    peer.go();

    peer.wait(&display, "icon_set");
    let events = take::<luminaria::protocol::ToplevelIconChange>();
    assert!(
        !events.is_empty(),
        "expected ToplevelIconChange: {events:?}"
    );
    let ev = &events[0];
    assert_eq!(ev.name, "my-test-app");
    assert_eq!(icon_mgr.icon_name(ev.surface), "my-test-app");
    peer.finish(&display);
}

// ----------------------------------------------------------------------------
// 2. Cursor Shape events and wiring into gpu::Cursor
// ----------------------------------------------------------------------------

fn client_cursor_shape(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let shm: wayland_client::protocol::wl_shm::WlShm = globals.bind(&qh, 1..=1, ()).unwrap();
    let wm: xdg_wm_base::XdgWmBase = globals.bind(&qh, 1..=6, ()).unwrap();
    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=7, ()).unwrap();
    let shape_mgr: wp_cursor_shape_manager_v1::WpCursorShapeManagerV1 =
        globals.bind(&qh, 1..=2, ()).unwrap();
    let mut state = State::default();

    let surface = compositor.create_surface(&qh, ());
    let xsurf = wm.get_xdg_surface(&surface, &qh, ());
    let _toplevel = xsurf.get_toplevel(&qh, ());
    let buffer = common::shm_buffer(&shm, &qh, 64, 64);
    surface.attach(Some(&buffer), 0, 0);
    surface.commit();

    let _keyboard = seat.get_keyboard(&qh, ());
    let pointer = seat.get_pointer(&qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("ready");

    queue.roundtrip(&mut state).unwrap();
    let device = shape_mgr.get_pointer(&pointer, &qh, ());
    let serial = state.pointer_enter_serial.expect("expected pointer enter");
    device.set_shape(serial, wp_cursor_shape_device_v1::Shape::Pointer);
    queue.roundtrip(&mut state).unwrap();
    step.step("shape_set");
}

#[test]
fn test_cursor_shape_events() {
    let display = Display::new().expect("display");
    display.init_shm().expect("shm");
    let _comp = Compositor::new(&display).expect("compositor");
    let shell = XdgShell::new(&display).expect("shell");
    let seat = Seat::new(&display, "seat0").expect("seat");
    seat.set_capabilities(true, true, false);
    let shape_mgr = CursorShapeManager::new(&display).expect("cursor_shape");

    let layout = OutputLayout::new().expect("layout");
    let renderer = SceneRenderer::new(&VulkanOptions::default()).expect("renderer");
    let _cursor = Cursor::new_with_shapes(&renderer, &layout, &seat, &shape_mgr).expect("cursor");

    let mut peer = Peer::spawn(&display, client_cursor_shape);

    peer.wait(&display, "ready");
    let mut surface = None;
    for ev in take::<luminaria::protocol::XdgEvent>() {
        if let luminaria::protocol::XdgEvent::Toplevel(id, _) = ev {
            surface = Some(id);
            if let Some(tl) = shell.toplevel(id) {
                tl.configure((320, 240));
            }
        }
    }
    let surface = surface.expect("expected client toplevel");
    seat.set_keyboard_focus(Some(surface));
    seat.pointer_enter(surface, 10.0, 10.0);
    peer.go();

    peer.wait(&display, "shape_set");
    let requests = take::<luminaria::protocol::CursorShapeRequest>();
    assert!(
        !requests.is_empty(),
        "expected CursorShapeRequest: {requests:?}"
    );
    let req = &requests[0];
    assert_eq!(req.name, "pointer");
    peer.finish(&display);
}

// ----------------------------------------------------------------------------
// 3. Activation token request
// ----------------------------------------------------------------------------

fn client_activation_token(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let activation: xdg_activation_v1::XdgActivationV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let surface = compositor.create_surface(&qh, ());
    surface.commit();

    let token = activation.get_activation_token(&qh, ());
    token.set_surface(&surface);
    token.set_app_id("org.example.CustomApp".into());
    token.commit();

    queue.roundtrip(&mut state).unwrap();
    step.step("token_committed");
}

#[test]
fn test_activation_token_events() {
    let display = Display::new().expect("display");
    let _comp = Compositor::new(&display).expect("compositor");
    let _activation = XdgActivation::new(&display).expect("activation");

    let mut peer = Peer::spawn(&display, client_activation_token);

    peer.wait(&display, "token_committed");
    let events = take::<luminaria::protocol::ActivationEvent>();
    assert!(!events.is_empty(), "expected TokenRequest: {events:?}");
    let token_req = events
        .iter()
        .find_map(|e| match e {
            ActivationEvent::TokenRequest(t) => Some(t),
            _ => None,
        })
        .expect("expected TokenRequest");
    assert_eq!(token_req.app_id, "org.example.CustomApp");
    assert!(token_req.granted);
    peer.finish(&display);
}

// ----------------------------------------------------------------------------
// 4. Workspace and Foreign Toplevel events
// ----------------------------------------------------------------------------

fn client_workspace_and_foreign(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let shm: wayland_client::protocol::wl_shm::WlShm = globals.bind(&qh, 1..=1, ()).unwrap();
    let wm: xdg_wm_base::XdgWmBase = globals.bind(&qh, 1..=6, ()).unwrap();
    let ws_mgr: ext_workspace_manager_v1::ExtWorkspaceManagerV1 =
        globals.bind(&qh, 1..=1, ()).unwrap();
    let _ft_mgr: zwlr_foreign_toplevel_manager_v1::ZwlrForeignToplevelManagerV1 =
        globals.bind(&qh, 1..=3, ()).unwrap();
    let mut state = State::default();

    // Workspaces: request create workspace in group
    queue.roundtrip(&mut state).unwrap();
    let group = state
        .group_handle
        .as_ref()
        .expect("expected workspace group handle");
    group.create_workspace("work-ws-1".into());
    ws_mgr.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("workspace_requested");

    // Foreign toplevel: create toplevel and attach buffer so server track() advertises it
    let surface = compositor.create_surface(&qh, ());
    let xsurf = wm.get_xdg_surface(&surface, &qh, ());
    let _toplevel = xsurf.get_toplevel(&qh, ());
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    let buffer = common::shm_buffer(&shm, &qh, 64, 64);
    surface.attach(Some(&buffer), 0, 0);
    surface.commit();
    queue.roundtrip(&mut state).unwrap();
    step.step("toplevel_created");

    // Client requests maximize on foreign toplevel handle
    let handle = state
        .foreign_handle
        .as_ref()
        .expect("expected foreign toplevel handle");
    handle.set_maximized();
    queue.roundtrip(&mut state).unwrap();
    step.step("foreign_maximized");
}

#[test]
fn test_workspace_and_foreign_toplevel_events() {
    let display = Display::new().expect("display");
    display.init_shm().expect("shm");
    let _comp = Compositor::new(&display).expect("compositor");
    let shell = XdgShell::new(&display).expect("shell");
    let mut ws_mgr = WorkspaceManager::new(&display).expect("workspace");
    ws_mgr.create_group();
    let mut ft_mgr = ForeignToplevelManager::new(&display).expect("foreign");
    ft_mgr.track(&shell);

    let mut peer = Peer::spawn(&display, client_workspace_and_foreign);

    peer.wait(&display, "workspace_requested");
    let ws_events = take::<luminaria::desktop::WorkspaceRequest>();
    assert!(
        !ws_events.is_empty(),
        "expected WorkspaceRequest: {ws_events:?}"
    );
    let ws_req = &ws_events[0];
    assert_eq!(ws_req.kind, WorkspaceRequestKind::Create);
    assert_eq!(ws_req.name, "work-ws-1");
    peer.go();

    peer.wait(&display, "toplevel_created");
    // Configure toplevel so it finishes mapping
    for ev in take::<luminaria::protocol::XdgEvent>() {
        if let luminaria::protocol::XdgEvent::Toplevel(id, _) = ev {
            if let Some(tl) = shell.toplevel(id) {
                tl.configure((320, 240));
            }
        }
    }
    peer.go();

    peer.wait(&display, "foreign_maximized");
    let ft_events = take::<luminaria::desktop::ForeignToplevelRequest>();
    assert!(
        !ft_events.is_empty(),
        "expected ForeignToplevelRequest: {ft_events:?}"
    );
    let ft_req = &ft_events[0];
    assert_eq!(ft_req.kind, ForeignToplevelRequestKind::Maximize);
    peer.finish(&display);
}

// ----------------------------------------------------------------------------
// 5. Output Management events
// ----------------------------------------------------------------------------

fn client_output_management(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let mgr: zwlr_output_manager_v1::ZwlrOutputManagerV1 = globals.bind(&qh, 1..=4, ()).unwrap();
    let mut state = State::default();

    queue.roundtrip(&mut state).unwrap();
    let serial = state.output_manager_serial.unwrap_or(1);

    // Create a configuration and apply it
    let config = mgr.create_configuration(serial, &qh, ());
    config.apply();
    queue.roundtrip(&mut state).unwrap();
    step.step("config_applied");
}

#[test]
fn test_output_management_events() {
    let display = Display::new().expect("display");
    let layout = OutputLayout::new().expect("layout");
    let _out_mgr = OutputManager::new(&display, &layout).expect("output manager");

    let mut peer = Peer::spawn(&display, client_output_management);

    peer.wait(&display, "config_applied");
    let requests = take::<luminaria::desktop::OutputConfigRequest>();
    assert!(
        !requests.is_empty(),
        "expected OutputConfigRequest: {requests:?}"
    );
    let req = &requests[0];
    assert!(!req.test); // applied, not test
    peer.finish(&display);
}
