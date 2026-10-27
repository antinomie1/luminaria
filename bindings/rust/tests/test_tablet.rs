//! Graphics tablets: the manager's device bookkeeping and the calls that
//! deliver a pen to a client, driven without a client attached (every `tool_*`
//! call with nobody under the pen must be a quiet no-op), and the event the
//! binding hands back when a client changes its tool's cursor.
use luminaria::core::Display;
use luminaria::protocol::{
    SurfaceId, TabletCursorRequest, TabletManager, TabletPadGroup, TabletPadLayout,
    TabletToolCaps, TabletToolType,
};
use luminaria::Event;

fn display() -> Display {
    let temp_dir = std::env::temp_dir().join("luminaria_test_tablet_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }
    Display::new().expect("failed to create display")
}

#[test]
fn devices_come_and_go() {
    let display = display();
    let tablets = TabletManager::new(&display).expect("TabletManager failed");

    let tablet = tablets.add_tablet("Test Tablet", 0x056a, 0x0378, "/dev/input/event9", 3);
    let tool = tablets.add_tool(
        TabletToolType::Pen,
        0x1234,
        0,
        TabletToolCaps {
            tilt: true,
            pressure: true,
            distance: true,
            ..Default::default()
        },
    );
    let pad = tablets.add_pad(
        "/dev/input/event10",
        &TabletPadLayout {
            buttons: 4,
            rings: 1,
            strips: 1,
            groups: vec![TabletPadGroup {
                modes: 2,
                buttons: vec![0, 1, 2, 3],
                rings: vec![0],
                strips: vec![0],
            }],
        },
    );
    assert!(tablet != 0 && tool != 0 && pad != 0);
    assert_eq!(
        (tablets.tablet_count(), tablets.tool_count(), tablets.pad_count()),
        (1, 1, 1)
    );

    // Nobody is under the pen: all of this is a no-op, not a crash.
    assert_eq!(tablets.tool_focus(tool), None);
    tablets.tool_proximity_in(tool, tablet, SurfaceId::INVALID);
    tablets.tool_motion(tool, 10.0, 20.0);
    tablets.tool_pressure(tool, 0.5);
    tablets.tool_tilt(tool, 10.0, -20.0);
    tablets.tool_button(tool, 0x14b, true);
    tablets.tool_frame(tool, 1);
    tablets.tool_proximity_out(tool);
    tablets.pad_enter(pad, tablet, SurfaceId::INVALID);
    tablets.pad_button(pad, 1, 2, true);
    tablets.pad_ring(pad, 0, 90.0, 1);
    tablets.pad_strip(pad, 0, 0.25, 1);
    tablets.pad_mode_switch(pad, 0, 1, 1);
    tablets.pad_leave(pad);
    assert_eq!(tablets.tool_focus(tool), None);

    tablets.remove_tool(tool);
    tablets.remove_tablet(tablet);
    tablets.remove_pad(pad);
    assert_eq!(
        (tablets.tablet_count(), tablets.tool_count(), tablets.pad_count()),
        (0, 0, 0)
    );
}

#[test]
fn caps_round_trip_and_cursor_event_converts() {
    let caps = TabletToolCaps {
        tilt: true,
        slider: true,
        wheel: true,
        ..Default::default()
    };
    assert_ne!(caps, TabletToolCaps::default());

    let request = TabletCursorRequest {
        tool: 3,
        serial: 7,
        surface: None,
        hotspot_x: 3,
        hotspot_y: 4,
    };
    let event: Event = request.into();
    let back: Result<TabletCursorRequest, Event> = event.try_into();
    assert_eq!(back.unwrap(), request);
}
