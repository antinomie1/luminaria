//! Graphics tablets: `zwp_tablet_manager_v2`.
//!
//! A tablet is three kinds of thing the protocol keeps apart: the **tablet** a
//! pen is held over, the **tool** (pen, eraser, puck) that moves between
//! tablets, and the **pad** of express keys, rings and strips beside it. The
//! compositor owns all three — [`TabletManager::add_tablet`], [`add_tool`](TabletManager::add_tool),
//! [`add_pad`](TabletManager::add_pad) — and every client that binds the global
//! is told about them.
//!
//! The backend reports them as [`TabletInput`] (inside
//! [`InputEvent::Tablet`](crate::backend::InputEvent::Tablet)); positions are
//! normalised to the tablet's active area, and mapping them onto the layout and
//! finding the surface underneath is yours. What comes out of that goes in
//! through the `tool_*` calls, which deliver to the client that owns the
//! surface the tool is over, and are closed by [`TabletManager::tool_frame`].
//!
//! A client changing the cursor its tool shows arrives as
//! [`Event::TabletCursor`](crate::Event::TabletCursor); a named shape
//! (`wp_cursor_shape`) as [`CursorShapeRequest`](crate::protocol::CursorShapeRequest)
//! with `tablet_tool` set.

use super::SurfaceId;
use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi;

/// What kind of tool it is. The values are evdev's `BTN_TOOL_*`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum TabletToolType {
    Pen = 0x140,
    Eraser = 0x141,
    Brush = 0x142,
    Pencil = 0x143,
    Airbrush = 0x144,
    Finger = 0x145,
    Mouse = 0x146,
    Lens = 0x147,
}

impl TabletToolType {
    pub(crate) fn from_u32(value: u32) -> Self {
        match value {
            0x141 => Self::Eraser,
            0x142 => Self::Brush,
            0x143 => Self::Pencil,
            0x144 => Self::Airbrush,
            0x145 => Self::Finger,
            0x146 => Self::Mouse,
            0x147 => Self::Lens,
            _ => Self::Pen,
        }
    }
}

/// The axes a tool reports beyond position. Position and buttons are universal.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
pub struct TabletToolCaps {
    pub tilt: bool,
    pub pressure: bool,
    pub distance: bool,
    pub rotation: bool,
    pub slider: bool,
    pub wheel: bool,
}

impl TabletToolCaps {
    pub(crate) fn from_bits(bits: u32) -> Self {
        Self {
            tilt: bits & 1 != 0,
            pressure: bits & 2 != 0,
            distance: bits & 4 != 0,
            rotation: bits & 8 != 0,
            slider: bits & 16 != 0,
            wheel: bits & 32 != 0,
        }
    }

    pub(crate) fn bits(self) -> u32 {
        u32::from(self.tilt)
            | u32::from(self.pressure) << 1
            | u32::from(self.distance) << 2
            | u32::from(self.rotation) << 3
            | u32::from(self.slider) << 4
            | u32::from(self.wheel) << 5
    }
}

/// One sample of a tool. `x`/`y` are normalised across the tablet; only the
/// axes whose `has_*` says so are meaningful in an update (a proximity-in
/// carries all of them).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct TabletAxes {
    changed: u32,
    /// (0,0) is the tablet's top-left, (1,1) its bottom-right.
    pub x: f64,
    pub y: f64,
    /// 0 to 1.
    pub pressure: f64,
    /// 0 to 1: how far above the surface.
    pub distance: f64,
    /// Degrees from the surface normal.
    pub tilt_x: f64,
    pub tilt_y: f64,
    /// Degrees clockwise about the tool's own axis.
    pub rotation: f64,
    /// -1 to 1.
    pub slider: f64,
    pub wheel_degrees: f64,
    pub wheel_clicks: i32,
}

impl TabletAxes {
    pub(crate) fn from_raw(raw: &ffi::TabletAxesRaw) -> Self {
        Self {
            changed: raw.changed,
            x: raw.x,
            y: raw.y,
            pressure: raw.pressure,
            distance: raw.distance,
            tilt_x: raw.tilt_x,
            tilt_y: raw.tilt_y,
            rotation: raw.rotation,
            slider: raw.slider,
            wheel_degrees: raw.wheel_degrees,
            wheel_clicks: raw.wheel_clicks,
        }
    }
    pub fn has_position(&self) -> bool {
        self.changed & 1 != 0
    }
    pub fn has_pressure(&self) -> bool {
        self.changed & 2 != 0
    }
    pub fn has_distance(&self) -> bool {
        self.changed & 4 != 0
    }
    pub fn has_tilt(&self) -> bool {
        self.changed & 8 != 0
    }
    pub fn has_rotation(&self) -> bool {
        self.changed & 16 != 0
    }
    pub fn has_slider(&self) -> bool {
        self.changed & 32 != 0
    }
    pub fn has_wheel(&self) -> bool {
        self.changed & 64 != 0
    }
}

/// How a pad's controls group under a mode switch.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct TabletPadGroup {
    /// How many modes the group cycles through.
    pub modes: u32,
    pub buttons: Vec<u32>,
    pub rings: Vec<u32>,
    pub strips: Vec<u32>,
}

/// What a pad has.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct TabletPadLayout {
    pub buttons: u32,
    pub rings: u32,
    pub strips: u32,
    pub groups: Vec<TabletPadGroup>,
}

/// A tablet or pad the backend found.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TabletDevice {
    pub device: u32,
    pub name: String,
    pub vid: u16,
    pub pid: u16,
    /// evdev `BUS_*`, 0 when unknown.
    pub bustype: u32,
    /// `/dev/input/eventN`.
    pub path: String,
    /// `Some` for a pad.
    pub pad: Option<TabletPadLayout>,
}

/// A tablet event from the backend, as [`InputEvent::Tablet`](crate::backend::InputEvent::Tablet).
/// Ids are the backend's: `device` names the tablet or pad, `tool` the pen.
#[derive(Debug, Clone, PartialEq)]
pub enum TabletInput {
    DeviceAdded(TabletDevice),
    DeviceRemoved {
        device: u32,
    },
    /// A tool came into range of (or left) `device`.
    Proximity {
        device: u32,
        tool: u64,
        inside: bool,
        tool_type: TabletToolType,
        /// The pen's hardware serial, 0 when it has none.
        serial: u64,
        wacom_id: u64,
        caps: TabletToolCaps,
        axes: TabletAxes,
        time_ms: u32,
    },
    Axis {
        device: u32,
        tool: u64,
        axes: TabletAxes,
        time_ms: u32,
    },
    /// The tip touched (`down`) or left the surface.
    Tip {
        device: u32,
        tool: u64,
        down: bool,
        axes: TabletAxes,
        time_ms: u32,
    },
    /// A barrel button (`BTN_STYLUS` and friends).
    Button {
        device: u32,
        tool: u64,
        button: u32,
        pressed: bool,
        time_ms: u32,
    },
    PadButton {
        device: u32,
        button: u32,
        pressed: bool,
        group: u32,
        mode: u32,
        time_ms: u32,
    },
    /// `angle` in degrees, negative when the finger lifted.
    PadRing {
        device: u32,
        ring: u32,
        angle: f64,
        time_ms: u32,
    },
    /// `position` 0 to 1, negative when the finger lifted.
    PadStrip {
        device: u32,
        strip: u32,
        position: f64,
        time_ms: u32,
    },
}

impl TabletInput {
    /// `None` for the input kinds that are not tablet events.
    pub(crate) fn from_raw(raw: &ffi::InputEventRaw) -> Option<Self> {
        use ffi::InputEventKind as K;
        Some(match raw.kind {
            K::TabletDeviceAdded => Self::DeviceAdded(TabletDevice {
                device: raw.device,
                name: raw.text.clone(),
                vid: raw.vid as u16,
                pid: raw.pid as u16,
                bustype: raw.bustype,
                path: raw.path.clone(),
                pad: (raw.flags & 1 != 0).then(|| TabletPadLayout {
                    buttons: raw.pad_buttons,
                    rings: raw.pad_rings,
                    strips: raw.pad_strips,
                    groups: raw
                        .groups
                        .iter()
                        .map(|g| TabletPadGroup {
                            modes: g.modes,
                            buttons: g.buttons.clone(),
                            rings: g.rings.clone(),
                            strips: g.strips.clone(),
                        })
                        .collect(),
                }),
            }),
            K::TabletDeviceRemoved => Self::DeviceRemoved { device: raw.device },
            K::TabletToolProximity => Self::Proximity {
                device: raw.device,
                tool: raw.tool,
                inside: raw.pressed,
                tool_type: TabletToolType::from_u32(raw.tool_type),
                serial: raw.serial,
                wacom_id: raw.wacom_id,
                caps: TabletToolCaps::from_bits(raw.caps),
                axes: TabletAxes::from_raw(&raw.axes),
                time_ms: raw.time,
            },
            K::TabletToolAxis => Self::Axis {
                device: raw.device,
                tool: raw.tool,
                axes: TabletAxes::from_raw(&raw.axes),
                time_ms: raw.time,
            },
            K::TabletToolTip => Self::Tip {
                device: raw.device,
                tool: raw.tool,
                down: raw.pressed,
                axes: TabletAxes::from_raw(&raw.axes),
                time_ms: raw.time,
            },
            K::TabletToolButton => Self::Button {
                device: raw.device,
                tool: raw.tool,
                button: raw.code,
                pressed: raw.pressed,
                time_ms: raw.time,
            },
            K::TabletPadButton => Self::PadButton {
                device: raw.device,
                button: raw.code,
                pressed: raw.pressed,
                group: raw.group,
                mode: raw.mode,
                time_ms: raw.time,
            },
            K::TabletPadRing => Self::PadRing {
                device: raw.device,
                ring: raw.code,
                angle: raw.x,
                time_ms: raw.time,
            },
            K::TabletPadStrip => Self::PadStrip {
                device: raw.device,
                strip: raw.code,
                position: raw.x,
                time_ms: raw.time,
            },
            _ => return None,
        })
    }
}

/// A tablet, tool or pad handle, handed out by the manager and never reused.
/// 0 is never valid.
pub type TabletId = u32;
pub type TabletToolId = u32;
pub type TabletPadId = u32;

/// A client set (or hid) the cursor its tablet tool shows: the focused client,
/// answering the serial it got at proximity-in.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TabletCursorRequest {
    pub tool: TabletToolId,
    pub serial: u32,
    /// `None` to hide the cursor.
    pub surface: Option<SurfaceId>,
    pub hotspot_x: i32,
    pub hotspot_y: i32,
}

impl TabletCursorRequest {
    pub(crate) fn from_raw(raw: crate::ffi::EventRaw) -> Option<Self> {
        match raw.kind {
            ffi::EventKind::TabletToolCursor => Some(Self {
                tool: raw.id as u32,
                serial: raw.serial,
                surface: SurfaceId::from_raw(raw.other),
                hotspot_x: raw.x,
                hotspot_y: raw.y,
            }),
            _ => None,
        }
    }
}

/// The `zwp_tablet_manager_v2` global (version 2).
pub struct TabletManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::TabletManagerShim>,
    _display: Holds<'d>,
}

impl<'d> TabletManager<'d> {
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::tablet_manager_new(&display.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }

    /// A tablet appeared. Every bound client is told at once.
    pub fn add_tablet(&self, name: &str, vid: u16, pid: u16, path: &str, bustype: u32) -> TabletId {
        self.raw.add_tablet(name, vid, pid, path, bustype)
    }
    /// It was unplugged. Tools over it leave first.
    pub fn remove_tablet(&self, tablet: TabletId) {
        self.raw.remove_tablet(tablet);
    }
    pub fn add_tool(
        &self,
        tool_type: TabletToolType,
        hardware_serial: u64,
        hardware_id_wacom: u64,
        caps: TabletToolCaps,
    ) -> TabletToolId {
        self.raw.add_tool(
            tool_type as u32,
            hardware_serial,
            hardware_id_wacom,
            caps.bits(),
        )
    }
    pub fn remove_tool(&self, tool: TabletToolId) {
        self.raw.remove_tool(tool);
    }
    pub fn add_pad(&self, path: &str, layout: &TabletPadLayout) -> TabletPadId {
        let groups: Vec<ffi::PadGroupRaw> = layout
            .groups
            .iter()
            .map(|g| ffi::PadGroupRaw {
                modes: g.modes,
                buttons: g.buttons.clone(),
                rings: g.rings.clone(),
                strips: g.strips.clone(),
            })
            .collect();
        self.raw
            .add_pad(path, layout.buttons, layout.rings, layout.strips, &groups)
    }
    pub fn remove_pad(&self, pad: TabletPadId) {
        self.raw.remove_pad(pad);
    }

    pub fn tablet_count(&self) -> u32 {
        self.raw.tablet_count()
    }
    pub fn tool_count(&self) -> u32 {
        self.raw.tool_count()
    }
    pub fn pad_count(&self) -> u32 {
        self.raw.pad_count()
    }

    /// The tool entered `surface`'s client's range over `tablet`. Follow it with
    /// the tool's current axes and a [`tool_frame`](Self::tool_frame).
    pub fn tool_proximity_in(&self, tool: TabletToolId, tablet: TabletId, surface: SurfaceId) {
        self.raw.tool_proximity_in(tool, tablet, surface.0);
    }
    /// The tool left (and closes its own frame).
    pub fn tool_proximity_out(&self, tool: TabletToolId) {
        self.raw.tool_proximity_out(tool);
    }
    /// The surface the tool is over.
    pub fn tool_focus(&self, tool: TabletToolId) -> Option<SurfaceId> {
        SurfaceId::from_raw(self.raw.tool_focus(tool))
    }
    pub fn tool_down(&self, tool: TabletToolId) {
        self.raw.tool_down(tool);
    }
    pub fn tool_up(&self, tool: TabletToolId) {
        self.raw.tool_up(tool);
    }
    /// Surface-local coordinates.
    pub fn tool_motion(&self, tool: TabletToolId, x: f64, y: f64) {
        self.raw.tool_motion(tool, x, y);
    }
    pub fn tool_pressure(&self, tool: TabletToolId, pressure: f64) {
        self.raw.tool_pressure(tool, pressure);
    }
    pub fn tool_distance(&self, tool: TabletToolId, distance: f64) {
        self.raw.tool_distance(tool, distance);
    }
    pub fn tool_tilt(&self, tool: TabletToolId, x_deg: f64, y_deg: f64) {
        self.raw.tool_tilt(tool, x_deg, y_deg);
    }
    pub fn tool_rotation(&self, tool: TabletToolId, degrees: f64) {
        self.raw.tool_rotation(tool, degrees);
    }
    pub fn tool_slider(&self, tool: TabletToolId, position: f64) {
        self.raw.tool_slider(tool, position);
    }
    pub fn tool_wheel(&self, tool: TabletToolId, degrees: f64, clicks: i32) {
        self.raw.tool_wheel(tool, degrees, clicks);
    }
    /// A barrel button (`BTN_STYLUS` = 0x14b, `BTN_STYLUS2` = 0x14c...).
    pub fn tool_button(&self, tool: TabletToolId, button: u32, pressed: bool) {
        self.raw.tool_button(tool, button, pressed);
    }
    /// Closes a group of changes: the client applies them as one sample.
    pub fn tool_frame(&self, tool: TabletToolId, time_ms: u32) {
        self.raw.tool_frame(tool, time_ms);
    }

    pub fn pad_enter(&self, pad: TabletPadId, tablet: TabletId, surface: SurfaceId) {
        self.raw.pad_enter(pad, tablet, surface.0);
    }
    pub fn pad_leave(&self, pad: TabletPadId) {
        self.raw.pad_leave(pad);
    }
    pub fn pad_button(&self, pad: TabletPadId, time_ms: u32, button: u32, pressed: bool) {
        self.raw.pad_button(pad, time_ms, button, pressed);
    }
    /// `angle` in degrees, or negative for "the finger lifted".
    pub fn pad_ring(&self, pad: TabletPadId, ring: u32, angle: f64, time_ms: u32) {
        self.raw.pad_ring(pad, ring, angle, time_ms);
    }
    /// `position` 0 to 1, or negative for "the finger lifted".
    pub fn pad_strip(&self, pad: TabletPadId, strip: u32, position: f64, time_ms: u32) {
        self.raw.pad_strip(pad, strip, position, time_ms);
    }
    pub fn pad_mode_switch(&self, pad: TabletPadId, group: u32, mode: u32, time_ms: u32) {
        self.raw.pad_mode_switch(pad, group, mode, time_ms);
    }
}
