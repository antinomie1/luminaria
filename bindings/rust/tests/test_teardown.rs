//! The teardown order CLAUDE.md asks of every protocol, through the binding:
//! a client still connected and holding one of every object the shims watch
//! while the globals drop, then the Display — which destroys the client and
//! its objects after every global is gone. The PopupTree and LayerManager are
//! declared before the Display, so they are still alive when their popup and
//! layer surface die inside its drop. Run it under ASan
//! (`LUMINARIA_SANITIZE=address`) to see anything here go wrong.

#[macro_use]
mod common;

use common::{drain, shm_buffer, ClientSide, Peer};
use luminaria::core::Display;
use luminaria::desktop::{InputMethodManager, SessionLockManager};
use luminaria::protocol::{
    Compositor, DataDeviceManager, IdleInhibitManager, IdleNotifier, LayerShell, LayerSurfaceEvent, OutputGlobal,
    PointerConstraints, PopupEvent, Seat, TextInputEvent, TextInputManager, XdgActivation, XdgEvent, XdgShell,
};
use luminaria::shell::{LayerManager, PopupTree};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_data_device, wl_data_device_manager, wl_data_offer, wl_data_source, wl_keyboard,
    wl_output, wl_pointer, wl_seat, wl_shm, wl_shm_pool, wl_surface,
};
use wayland_client::{event_created_child, Connection, Dispatch, QueueHandle};
use wayland_protocols::ext::idle_notify::v1::client::{ext_idle_notification_v1, ext_idle_notifier_v1};
use wayland_protocols::ext::session_lock::v1::client::{
    ext_session_lock_manager_v1, ext_session_lock_surface_v1, ext_session_lock_v1,
};
use wayland_protocols::wp::idle_inhibit::zv1::client::{zwp_idle_inhibit_manager_v1, zwp_idle_inhibitor_v1};
use wayland_protocols::wp::pointer_constraints::zv1::client::{zwp_locked_pointer_v1, zwp_pointer_constraints_v1};
use wayland_protocols::wp::text_input::zv3::client::{zwp_text_input_manager_v3, zwp_text_input_v3};
use wayland_protocols::xdg::activation::v1::client::{xdg_activation_token_v1, xdg_activation_v1};
use wayland_protocols::xdg::shell::client::{xdg_popup, xdg_positioner, xdg_wm_base};
use wayland_protocols_misc::zwp_input_method_v2::client::{
    zwp_input_method_keyboard_grab_v2, zwp_input_method_manager_v2, zwp_input_method_v2, zwp_input_popup_surface_v2,
};
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};

struct State;

registry_state!(State);
xdg_basics!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_shm::WlShm,
    wl_shm_pool::WlShmPool,
    wl_buffer::WlBuffer,
    wl_output::WlOutput,
    wl_seat::WlSeat,
    wl_pointer::WlPointer,
    wl_keyboard::WlKeyboard,
    wl_data_device_manager::WlDataDeviceManager,
    wl_data_offer::WlDataOffer,
    wl_data_source::WlDataSource,
    xdg_positioner::XdgPositioner,
    xdg_popup::XdgPopup,
    xdg_activation_v1::XdgActivationV1,
    xdg_activation_token_v1::XdgActivationTokenV1,
    zwlr_layer_shell_v1::ZwlrLayerShellV1,
    zwlr_layer_surface_v1::ZwlrLayerSurfaceV1,
    ext_session_lock_manager_v1::ExtSessionLockManagerV1,
    ext_session_lock_v1::ExtSessionLockV1,
    ext_session_lock_surface_v1::ExtSessionLockSurfaceV1,
    zwp_idle_inhibit_manager_v1::ZwpIdleInhibitManagerV1,
    zwp_idle_inhibitor_v1::ZwpIdleInhibitorV1,
    ext_idle_notifier_v1::ExtIdleNotifierV1,
    ext_idle_notification_v1::ExtIdleNotificationV1,
    zwp_pointer_constraints_v1::ZwpPointerConstraintsV1,
    zwp_locked_pointer_v1::ZwpLockedPointerV1,
    zwp_text_input_manager_v3::ZwpTextInputManagerV3,
    zwp_text_input_v3::ZwpTextInputV3,
    zwp_input_method_manager_v2::ZwpInputMethodManagerV2,
    zwp_input_method_v2::ZwpInputMethodV2,
    zwp_input_method_keyboard_grab_v2::ZwpInputMethodKeyboardGrabV2,
    zwp_input_popup_surface_v2::ZwpInputPopupSurfaceV2,
);

impl Dispatch<wl_data_device::WlDataDevice, ()> for State {
    fn event(_: &mut Self, _: &wl_data_device::WlDataDevice, _: wl_data_device::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}

    event_created_child!(State, wl_data_device::WlDataDevice, [
        wl_data_device::EVT_DATA_OFFER_OPCODE => (wl_data_offer::WlDataOffer, ()),
    ]);
}

fn client(conn: Connection, step: ClientSide) {
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let shm: wl_shm::WlShm = globals.bind(&qh, 1..=1, ()).unwrap();
    let output: wl_output::WlOutput = globals.bind(&qh, 1..=4, ()).unwrap();
    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=7, ()).unwrap();
    let wm: xdg_wm_base::XdgWmBase = globals.bind(&qh, 1..=6, ()).unwrap();
    let mut state = State;

    // A window with a popup.
    let window = compositor.create_surface(&qh, ());
    let window_xdg = wm.get_xdg_surface(&window, &qh, ());
    let _toplevel = window_xdg.get_toplevel(&qh, ());
    window.commit();
    queue.roundtrip(&mut state).unwrap();
    let buffer = shm_buffer(&shm, &qh, 32, 32);
    window.attach(Some(&buffer), 0, 0);
    window.commit();
    let positioner = wm.create_positioner(&qh, ());
    positioner.set_size(10, 10);
    positioner.set_anchor_rect(0, 0, 1, 1);
    let menu = compositor.create_surface(&qh, ());
    let menu_xdg = wm.get_xdg_surface(&menu, &qh, ());
    let _popup = menu_xdg.get_popup(Some(&window_xdg), &positioner, &qh, ());
    menu.commit();

    // A panel.
    let layers: zwlr_layer_shell_v1::ZwlrLayerShellV1 = globals.bind(&qh, 1..=4, ()).unwrap();
    let panel = compositor.create_surface(&qh, ());
    let layer = layers.get_layer_surface(&panel, None, zwlr_layer_shell_v1::Layer::Top, "panel".into(), &qh, ());
    layer.set_size(10, 10);
    panel.commit();

    // A lock with its surface.
    let locks: ext_session_lock_manager_v1::ExtSessionLockManagerV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let lock = locks.lock(&qh, ());
    let lock_surface = compositor.create_surface(&qh, ());
    let _lock_surface = lock.get_lock_surface(&lock_surface, &output, &qh, ());

    // Idle.
    let inhibits: zwp_idle_inhibit_manager_v1::ZwpIdleInhibitManagerV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let _inhibitor = inhibits.create_inhibitor(&window, &qh, ());
    let notifier: ext_idle_notifier_v1::ExtIdleNotifierV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let _notification = notifier.get_idle_notification(60_000, &seat, &qh, ());

    // An activation token, a selection, a pointer lock.
    let activation: xdg_activation_v1::XdgActivationV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let token = activation.get_activation_token(&qh, ());
    token.commit();
    let devices: wl_data_device_manager::WlDataDeviceManager = globals.bind(&qh, 1..=3, ()).unwrap();
    let device = devices.get_data_device(&seat, &qh, ());
    let source = devices.create_data_source(&qh, ());
    source.offer("text/plain".into());
    device.set_selection(Some(&source), 0);
    let pointer = seat.get_pointer(&qh, ());
    let constraints: zwp_pointer_constraints_v1::ZwpPointerConstraintsV1 = globals.bind(&qh, 1..=1, ()).unwrap();
    let _locked = constraints.lock_pointer(&window, &pointer, None, zwp_pointer_constraints_v1::Lifetime::Persistent, &qh, ());

    // Text input and an input method with its grab and popup.
    let _keyboard = seat.get_keyboard(&qh, ());
    let text_inputs: zwp_text_input_manager_v3::ZwpTextInputManagerV3 = globals.bind(&qh, 1..=1, ()).unwrap();
    let text_input = text_inputs.get_text_input(&seat, &qh, ());
    text_input.enable();
    text_input.commit();
    let input_methods: zwp_input_method_manager_v2::ZwpInputMethodManagerV2 = globals.bind(&qh, 1..=1, ()).unwrap();
    let im = input_methods.get_input_method(&seat, &qh, ());
    let _grab = im.grab_keyboard(&qh, ());
    let candidates = compositor.create_surface(&qh, ());
    let _candidates = im.get_input_popup_surface(&candidates, &qh, ());
    queue.roundtrip(&mut state).unwrap();

    step.step("holding");
    // The server is gone by now.
    let _ = queue.roundtrip(&mut state);
}

#[test]
fn globals_and_display_drop_under_a_connected_client() {
    // Before the Display: alive while the Display's drop destroys the client.
    let mut popups = PopupTree::new().expect("popup tree");
    let mut layer_manager = LayerManager::new().expect("layer manager");
    let display = Display::new().expect("display");
    display.init_shm().expect("wl_shm");
    let compositor = Compositor::new(&display).expect("wl_compositor");
    let output = OutputGlobal::new(&display, 640, 480, "test-1").expect("wl_output");
    let seat = Seat::new(&display, "seat0").expect("wl_seat");
    seat.set_capabilities(true, true, false);
    let shell = XdgShell::new(&display).expect("xdg-shell");
    let layers = LayerShell::new(&display).expect("layer-shell");
    let locks = SessionLockManager::new(&display).expect("session-lock");
    let inhibits = IdleInhibitManager::new(&display).expect("idle-inhibit");
    let notifier = IdleNotifier::new(&display).expect("idle-notify");
    let activation = XdgActivation::new(&display).expect("xdg-activation");
    let clipboard = DataDeviceManager::new(&display, &seat).expect("data-device");
    let constraints = PointerConstraints::new(&display, &seat).expect("pointer-constraints");
    let text_inputs = TextInputManager::new(&display, &seat).expect("text-input");
    let input_methods = InputMethodManager::new(&display, &seat, &text_inputs).expect("input-method");

    let mut peer = Peer::spawn(&display, client);
    peer.wait(&display, "holding");

    let mut window = None;
    for event in drain(|| shell.next_event()) {
        match event {
            XdgEvent::Toplevel(s, _) => window = Some(s),
            XdgEvent::Popup(p, PopupEvent::Created { .. }) => assert!(popups.add(&shell, p)),
            _ => {}
        }
    }
    let window = window.expect("no toplevel");
    for (s, event) in drain(|| layers.next_event()) {
        if event == LayerSurfaceEvent::Created {
            assert!(layer_manager.add(&layers, s, 1));
        }
    }
    seat.set_keyboard_focus(Some(window));
    seat.pointer_enter(window, 1.0, 1.0);
    for _ in 0..4 {
        common::dispatch(&display);
    }
    assert!(locks.current().is_some());
    assert_eq!(inhibits.inhibitor_count(), 1);
    assert_eq!(notifier.notification_count(), 1);
    assert!(constraints.constraint(window).is_some());
    assert!(matches!(drain(|| text_inputs.next_event())[..], [TextInputEvent::New(_), ..]));
    assert!(input_methods.current().is_some());
    assert_eq!(clipboard.selection_mime_types(), ["text/plain"]);

    // Every global first, in reverse order of creation, then the Display.
    drop(input_methods);
    drop(text_inputs);
    drop(constraints);
    drop(clipboard);
    drop(activation);
    drop(notifier);
    drop(inhibits);
    drop(locks);
    drop(layers);
    drop(shell);
    drop(seat);
    drop(output);
    drop(compositor);
    drop(display);
    peer.join();

    // They let go of their popup and layer surface when the client died.
    assert!(!popups.dismiss_grabs_outside(None));
    assert_eq!(layer_manager.keyboard_target(window), None);
    layer_manager.close_output(1);
}
