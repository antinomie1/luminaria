use luminaria::core::Display;
use luminaria::desktop::{
    CtmControlManager, CtmEvent, GammaControlManager, GammaEvent, VirtualKeyEvent,
    VirtualKeyboardManager, VirtualModifiersEvent, VirtualPointerEvent, VirtualPointerManager,
};
use luminaria::protocol::{PointerGestures, SurfaceId};
use luminaria::Event;

#[test]
fn test_new_protocols_initialization_and_methods() {
    let temp_dir = std::env::temp_dir().join("luminaria_test_new_protocols_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }

    let display = Display::new().expect("failed to create display");

    // 1. PointerGestures (core / protocol)
    let gestures = PointerGestures::new(&display).expect("PointerGestures failed");
    let dummy_surface = SurfaceId::INVALID;

    // Swipe
    gestures.send_swipe_begin(dummy_surface, 1, 100, 3);
    gestures.send_swipe_update(110, 10.0, -5.0);
    gestures.send_swipe_end(2, 120, false);

    // Pinch
    gestures.send_pinch_begin(dummy_surface, 3, 200, 2);
    gestures.send_pinch_update(210, 0.0, 0.0, 1.5, 45.0);
    gestures.send_pinch_end(4, 220, false);

    // Hold
    gestures.send_hold_begin(dummy_surface, 5, 300, 4);
    gestures.send_hold_end(6, 310, false);

    // 2. GammaControlManager (desktop)
    let gamma_mgr = GammaControlManager::new(&display, 256).expect("GammaControlManager failed");
    assert_eq!(gamma_mgr.gamma_size(), 256);
    assert_eq!(gamma_mgr.ramp(0), None);

    // 3. CtmControlManager (desktop)
    let ctm_mgr = CtmControlManager::new(&display).expect("CtmControlManager failed");
    assert_eq!(ctm_mgr.matrix(0), None);

    // 4. VirtualKeyboardManager (desktop)
    let _vk_mgr = VirtualKeyboardManager::new(&display).expect("VirtualKeyboardManager failed");

    // 5. VirtualPointerManager (desktop)
    let _vp_mgr = VirtualPointerManager::new(&display).expect("VirtualPointerManager failed");
}

#[test]
fn test_new_protocols_events_roundtrip() {
    // Test event conversions
    let gamma_ev = GammaEvent {
        output_resource: 42,
        has_ramp: true,
    };
    let event: Event = gamma_ev.clone().into();
    let back: Result<GammaEvent, Event> = event.try_into();
    assert_eq!(back.unwrap(), gamma_ev);

    let ctm_ev = CtmEvent {
        output_resource: 42,
    };
    let event: Event = ctm_ev.clone().into();
    let back: Result<CtmEvent, Event> = event.try_into();
    assert_eq!(back.unwrap(), ctm_ev);

    let vk_key_ev = VirtualKeyEvent {
        seat_resource: 10,
        time: 100,
        key: 30,
        pressed: true,
    };
    let event: Event = vk_key_ev.into();
    let back: Result<VirtualKeyEvent, Event> = event.try_into();
    assert_eq!(back.unwrap(), vk_key_ev);

    let vk_mod_ev = VirtualModifiersEvent {
        seat_resource: 10,
        depressed: 4,
        latched: 0,
        locked: 0,
        group: 0,
    };
    let event: Event = vk_mod_ev.into();
    let back: Result<VirtualModifiersEvent, Event> = event.try_into();
    assert_eq!(back.unwrap(), vk_mod_ev);

    let vp_ev = VirtualPointerEvent::Motion {
        seat_resource: 10,
        time: 200,
        dx: 15.0,
        dy: -8.0,
    };
    let event: Event = vp_ev.clone().into();
    let back: Result<VirtualPointerEvent, Event> = event.try_into();
    assert_eq!(back.unwrap(), vp_ev);
}
