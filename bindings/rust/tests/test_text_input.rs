//! text-input-v3 and input-method-v2 events, driven by a real client that is
//! both the text field and the input method: focus, enable and commit with
//! the state they carry, the answers sent back, the IM's activation, keyboard
//! grab and candidate popup, then disable and destruction.

#[macro_use]
mod common;

use common::{ClientSide, Peer, take};
use luminaria::core::Display;
use luminaria::desktop::{InputMethodEvent, InputMethodManager};
use luminaria::protocol::{
    Compositor, Seat, TextInputEvent, TextInputManager, ToplevelEvent, XdgEvent, XdgShell,
};
use luminaria::util::Rect;
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::{wl_compositor, wl_keyboard, wl_seat, wl_surface};
use wayland_client::{Connection, Dispatch, QueueHandle, WEnum};
use wayland_protocols::wp::text_input::zv3::client::{
    zwp_text_input_manager_v3, zwp_text_input_v3,
};
use wayland_protocols::xdg::shell::client::xdg_wm_base;
use wayland_protocols_misc::zwp_input_method_v2::client::{
    zwp_input_method_keyboard_grab_v2, zwp_input_method_manager_v2, zwp_input_method_v2,
    zwp_input_popup_surface_v2,
};

#[derive(Default)]
struct State {
    entered: bool,
    preedit: Option<String>,
    committed: Option<String>,
    /// The serial of the last text-input done.
    done: Option<u32>,
    im_activated: usize,
    im_surrounding: Option<String>,
    grab_keymap: bool,
    grab_keys: Vec<(u32, bool)>,
}

registry_state!(State);
xdg_basics!(State);
ignore_events!(State:
    wl_compositor::WlCompositor,
    wl_surface::WlSurface,
    wl_seat::WlSeat,
    zwp_text_input_manager_v3::ZwpTextInputManagerV3,
    zwp_input_method_manager_v2::ZwpInputMethodManagerV2,
    zwp_input_popup_surface_v2::ZwpInputPopupSurfaceV2,
);

impl Dispatch<zwp_text_input_v3::ZwpTextInputV3, ()> for State {
    fn event(
        state: &mut Self,
        _: &zwp_text_input_v3::ZwpTextInputV3,
        event: zwp_text_input_v3::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwp_text_input_v3::Event::Enter { .. } => state.entered = true,
            zwp_text_input_v3::Event::PreeditString { text, .. } => state.preedit = text,
            zwp_text_input_v3::Event::CommitString { text } => state.committed = text,
            zwp_text_input_v3::Event::Done { serial } => state.done = Some(serial),
            _ => {}
        }
    }
}

impl Dispatch<zwp_input_method_v2::ZwpInputMethodV2, ()> for State {
    fn event(
        state: &mut Self,
        _: &zwp_input_method_v2::ZwpInputMethodV2,
        event: zwp_input_method_v2::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwp_input_method_v2::Event::Activate => state.im_activated += 1,
            zwp_input_method_v2::Event::SurroundingText { text, .. } => {
                state.im_surrounding = Some(text)
            }
            _ => {}
        }
    }
}

impl Dispatch<zwp_input_method_keyboard_grab_v2::ZwpInputMethodKeyboardGrabV2, ()> for State {
    fn event(
        state: &mut Self,
        _: &zwp_input_method_keyboard_grab_v2::ZwpInputMethodKeyboardGrabV2,
        event: zwp_input_method_keyboard_grab_v2::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwp_input_method_keyboard_grab_v2::Event::Keymap { .. } => state.grab_keymap = true,
            zwp_input_method_keyboard_grab_v2::Event::Key {
                key,
                state: key_state,
                ..
            } => {
                state.grab_keys.push((
                    key,
                    key_state == WEnum::Value(wl_keyboard::KeyState::Pressed),
                ));
            }
            _ => {}
        }
    }
}

fn client(conn: Connection, step: ClientSide) {
    use zwp_text_input_v3::{ContentHint, ContentPurpose};
    let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
    let qh = queue.handle();
    let compositor: wl_compositor::WlCompositor = globals.bind(&qh, 1..=6, ()).unwrap();
    let wm: xdg_wm_base::XdgWmBase = globals.bind(&qh, 1..=6, ()).unwrap();
    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=7, ()).unwrap();
    let text_inputs: zwp_text_input_manager_v3::ZwpTextInputManagerV3 =
        globals.bind(&qh, 1..=1, ()).unwrap();
    let input_methods: zwp_input_method_manager_v2::ZwpInputMethodManagerV2 =
        globals.bind(&qh, 1..=1, ()).unwrap();
    let mut state = State::default();

    let surface = compositor.create_surface(&qh, ());
    let xs = wm.get_xdg_surface(&surface, &qh, ());
    let _toplevel = xs.get_toplevel(&qh, ());
    surface.commit();
    let im = input_methods.get_input_method(&seat, &qh, ());
    let ti = text_inputs.get_text_input(&seat, &qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("ready");

    queue.roundtrip(&mut state).unwrap();
    assert!(state.entered, "no enter after keyboard focus");
    ti.enable();
    ti.set_surrounding_text("hello".into(), 2, 2);
    ti.set_content_type(ContentHint::Spellcheck, ContentPurpose::Email);
    ti.set_cursor_rectangle(10, 20, 1, 15);
    ti.commit();
    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.im_activated, 1);
    assert_eq!(state.im_surrounding.as_deref(), Some("hello"));
    step.step("enabled");

    queue.roundtrip(&mut state).unwrap();
    assert_eq!(state.preedit.as_deref(), Some("wo"));
    assert_eq!(state.committed.as_deref(), Some("world"));
    assert_eq!(state.done, Some(1), "done carries the commit count");
    let grab = im.grab_keyboard(&qh, ());
    let candidates = compositor.create_surface(&qh, ());
    let popup = im.get_input_popup_surface(&candidates, &qh, ());
    queue.roundtrip(&mut state).unwrap();
    step.step("grabbed");

    queue.roundtrip(&mut state).unwrap();
    assert!(state.grab_keymap, "a grab comes with the keymap");
    assert_eq!(state.grab_keys, [(30, true)]);
    ti.disable();
    ti.commit();
    popup.destroy();
    grab.release();
    queue.roundtrip(&mut state).unwrap();
    step.step("disabled");

    im.destroy();
    ti.destroy();
    queue.roundtrip(&mut state).unwrap();
    step.step("destroyed");
}

#[test]
fn text_input_and_input_method() {
    let display = Display::new().expect("display");
    let _compositor = Compositor::new(&display).expect("wl_compositor");
    let _shell = XdgShell::new(&display).expect("xdg-shell");
    let seat = Seat::new(&display, "seat0").expect("wl_seat");
    seat.set_capabilities(true, false, false);
    let text_inputs = TextInputManager::new(&display, &seat).expect("text-input");
    let input_methods =
        InputMethodManager::new(&display, &seat, &text_inputs).expect("input-method");

    let mut peer = Peer::spawn(&display, client);
    peer.wait(&display, "ready");
    let s = match take::<XdgEvent>().first() {
        Some(&XdgEvent::Toplevel(s, ToplevelEvent::Created)) => s,
        other => panic!("{other:?}"),
    };
    let [TextInputEvent::New(t)] = take::<TextInputEvent>()[..] else {
        panic!("no text input");
    };
    let [InputMethodEvent::New(m)] = take::<InputMethodEvent>()[..] else {
        panic!("no input method");
    };
    assert_eq!(input_methods.current().map(|im| im.id()), Some(m));
    assert!(!input_methods.input_method(m).is_active());
    assert_eq!(text_inputs.focused(), None);
    seat.set_keyboard_focus(Some(s));
    peer.go();

    peer.wait(&display, "enabled");
    assert_eq!(
        take::<TextInputEvent>(),
        [TextInputEvent::Enable(t), TextInputEvent::Commit(t)]
    );
    assert_eq!(text_inputs.focused(), Some(t));
    let ti = text_inputs.text_input(t).expect("text input");
    let state = ti.state();
    assert_eq!(state.focused_surface, Some(s));
    assert!(state.enabled);
    assert_eq!((state.cursor, state.anchor), (2, 2));
    assert_eq!(state.content_hint, 2, "spellcheck");
    assert_eq!(state.content_purpose, 6, "email");
    assert_eq!(state.cursor_rectangle, Rect::new(10, 20, 1, 15));
    assert_eq!(state.serial, 1);
    assert_eq!(ti.surrounding_text(), "hello");
    assert!(input_methods.input_method(m).is_active());
    ti.send_preedit_string("wo", 0, 2);
    ti.send_commit_string("world");
    ti.send_done();
    peer.go();

    peer.wait(&display, "grabbed");
    let events = take::<InputMethodEvent>();
    let [
        InputMethodEvent::GrabChange {
            input_method,
            grabbed: true,
        },
        InputMethodEvent::NewPopupSurface {
            input_method: owner,
            surface: p,
        },
    ] = events[..]
    else {
        panic!("{events:?}");
    };
    assert_eq!((input_method, owner), (m, m));
    let im = input_methods.input_method(m);
    assert!(im.has_keyboard_grab());
    assert_eq!(im.popup_surfaces(), [p]);
    assert_eq!(
        input_methods.popup_text_input_rectangle(p),
        Rect::new(10, 20, 1, 15)
    );
    im.grab_send_key(100, 30, true);
    peer.go();

    peer.wait(&display, "disabled");
    assert_eq!(
        take::<TextInputEvent>(),
        [TextInputEvent::Disable(t), TextInputEvent::Commit(t)]
    );
    let events = take::<InputMethodEvent>();
    assert!(
        events.contains(&InputMethodEvent::PopupSurfaceDestroy(p)),
        "{events:?}"
    );
    assert!(
        events.contains(&InputMethodEvent::GrabChange {
            input_method: m,
            grabbed: false
        }),
        "{events:?}"
    );
    assert!(!im.is_active() && !im.has_keyboard_grab());
    peer.go();

    peer.wait(&display, "destroyed");
    assert_eq!(take::<InputMethodEvent>(), [InputMethodEvent::Destroy(m)]);
    assert_eq!(take::<TextInputEvent>(), [TextInputEvent::Destroy(t)]);
    assert!(input_methods.current().is_none());
    assert!(text_inputs.text_input(t).is_none());
    peer.finish(&display);
}
