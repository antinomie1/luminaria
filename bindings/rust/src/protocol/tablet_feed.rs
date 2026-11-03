//! [`TabletFeed`]: backend tablet events into [`TabletManager`] calls, as the
//! C++ `TabletManager::feed()` does, with the hit test a Rust closure.

use super::{
    SurfaceId, TabletAxes, TabletId, TabletInput, TabletManager, TabletPadId, TabletToolCaps,
    TabletToolId,
};
use crate::util::Rect;
use std::collections::HashMap;

/// What lies under a tool: the surface, and the point in its coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TabletTarget {
    pub surface: SurfaceId,
    pub x: f64,
    pub y: f64,
}

#[derive(Default)]
struct Tool {
    id: TabletToolId,
    tablet: TabletId,
    caps: TabletToolCaps,
    in_range: bool,
    down: bool,
    x: f64,
    y: f64,
}

/// The backend's tablets, pads and tools as the protocol knows them, and where
/// each tool is.
///
/// ```ignore
/// if let InputEvent::Tablet(input) = &event {
///     feed.feed(&tablets, input, layout_box, |x, y| hit_test(x, y));
/// }
/// ```
#[derive(Default)]
pub struct TabletFeed {
    tablets: HashMap<u32, TabletId>,
    pads: HashMap<u32, TabletPadId>,
    tools: HashMap<u64, Tool>,
    /// (pad device, group) → mode, to announce a switch before its button.
    modes: HashMap<(u32, u32), u32>,
}

impl TabletFeed {
    pub fn new() -> Self {
        Self::default()
    }

    /// Where the tool `backend_tool` is in the layout, while it is in range.
    pub fn tool_position(&self, backend_tool: u64) -> Option<(f64, f64)> {
        let tool = self.tools.get(&backend_tool)?;
        tool.in_range.then_some((tool.x, tool.y))
    }

    /// Applies one backend event. `area` is the layout rectangle the tablet's
    /// active area maps onto (usually one output); `locate` answers what is
    /// under a layout point. A tool crossing between surfaces gets proximity
    /// out and in, and pads follow whichever surface a tool is over.
    pub fn feed(
        &mut self,
        manager: &TabletManager<'_>,
        input: &TabletInput,
        area: Rect,
        mut locate: impl FnMut(f64, f64) -> Option<TabletTarget>,
    ) {
        match input {
            TabletInput::DeviceAdded(device) => match &device.pad {
                Some(layout) => {
                    self.pads
                        .entry(device.device)
                        .or_insert_with(|| manager.add_pad(&device.path, layout));
                }
                None => {
                    self.tablets.entry(device.device).or_insert_with(|| {
                        manager.add_tablet(
                            &device.name,
                            device.vid,
                            device.pid,
                            &device.path,
                            device.bustype,
                        )
                    });
                }
            },
            TabletInput::DeviceRemoved { device } => {
                if let Some(gone) = self.tablets.remove(device) {
                    for tool in self.tools.values_mut().filter(|t| t.tablet == gone) {
                        tool.in_range = false;
                    }
                    manager.remove_tablet(gone);
                } else if let Some(pad) = self.pads.remove(device) {
                    manager.remove_pad(pad);
                    self.modes.retain(|(d, _), _| d != device);
                }
            }
            TabletInput::Proximity {
                device,
                tool,
                inside,
                tool_type,
                serial,
                wacom_id,
                caps,
                axes,
                time_ms,
            } => {
                let Some(&tablet) = self.tablets.get(device) else {
                    return;
                };
                let entry = self.tools.entry(*tool).or_default();
                if entry.id == 0 {
                    entry.id = manager.add_tool(*tool_type, *serial, *wacom_id, *caps);
                    entry.caps = *caps;
                }
                entry.tablet = tablet;
                if !*inside {
                    entry.in_range = false;
                    if entry.down {
                        manager.tool_up(entry.id);
                        entry.down = false;
                    }
                    if manager.tool_focus(entry.id).is_some() {
                        manager.tool_proximity_out(entry.id);
                        self.sync_pads(manager, tablet);
                    }
                    return;
                }
                entry.in_range = true;
                let target = self.retarget(manager, *tool, axes, area, &mut locate);
                self.send_axes(manager, *tool, axes, target, true, *time_ms);
            }
            TabletInput::Axis {
                tool,
                axes,
                time_ms,
                ..
            } => {
                let Some(entry) = self.tools.get(tool).filter(|t| t.in_range) else {
                    return;
                };
                let was = manager.tool_focus(entry.id).is_some();
                let target = self.retarget(manager, *tool, axes, area, &mut locate);
                self.send_axes(manager, *tool, axes, target, !was, *time_ms);
            }
            TabletInput::Tip {
                tool,
                down,
                axes,
                time_ms,
                ..
            } => {
                if !self.tools.get(tool).is_some_and(|t| t.in_range) {
                    return;
                }
                let Some(target) = self.retarget(manager, *tool, axes, area, &mut locate) else {
                    return;
                };
                // Motion first, so the client sees where the tip came down.
                self.send_axes(manager, *tool, axes, Some(target), false, *time_ms);
                let entry = self.tools.get_mut(tool).expect("present");
                if *down && !entry.down {
                    manager.tool_down(entry.id);
                } else if !*down && entry.down {
                    manager.tool_up(entry.id);
                }
                entry.down = *down;
                manager.tool_frame(entry.id, *time_ms);
            }
            TabletInput::Button {
                tool,
                button,
                pressed,
                time_ms,
                ..
            } => {
                if let Some(entry) = self.tools.get(tool).filter(|t| t.in_range) {
                    manager.tool_button(entry.id, *button, *pressed);
                    manager.tool_frame(entry.id, *time_ms);
                }
            }
            TabletInput::PadButton {
                device,
                button,
                pressed,
                group,
                mode,
                time_ms,
            } => {
                let Some(&pad) = self.pads.get(device) else {
                    return;
                };
                let current = self.modes.entry((*device, *group)).or_insert(0);
                if current != mode {
                    *current = *mode;
                    manager.pad_mode_switch(pad, *group, *mode, *time_ms);
                }
                manager.pad_button(pad, *time_ms, *button, *pressed);
            }
            TabletInput::PadRing {
                device,
                ring,
                angle,
                time_ms,
            } => {
                if let Some(&pad) = self.pads.get(device) {
                    manager.pad_ring(pad, *ring, *angle, *time_ms);
                }
            }
            TabletInput::PadStrip {
                device,
                strip,
                position,
                time_ms,
            } => {
                if let Some(&pad) = self.pads.get(device) {
                    manager.pad_strip(pad, *strip, *position, *time_ms);
                }
            }
        }
    }

    /// While a tool is over a surface, every pad is focused on it.
    fn sync_pads(&self, manager: &TabletManager<'_>, over: TabletId) {
        let focus = self.tools.values().find_map(|t| manager.tool_focus(t.id));
        for &pad in self.pads.values() {
            match focus {
                Some(surface) => manager.pad_enter(pad, over, surface),
                None => manager.pad_leave(pad),
            }
        }
    }

    /// Moves the tool where `axes` says, sending proximity as it crosses
    /// between surfaces; the target it ends up over.
    fn retarget(
        &mut self,
        manager: &TabletManager<'_>,
        backend_tool: u64,
        axes: &TabletAxes,
        area: Rect,
        locate: &mut impl FnMut(f64, f64) -> Option<TabletTarget>,
    ) -> Option<TabletTarget> {
        let tool = self.tools.get_mut(&backend_tool)?;
        if axes.has_position() {
            tool.x = f64::from(area.x) + axes.x * f64::from(area.width);
            tool.y = f64::from(area.y) + axes.y * f64::from(area.height);
        }
        let (id, tablet) = (tool.id, tool.tablet);
        let target = if tool.in_range {
            locate(tool.x, tool.y)
        } else {
            None
        };
        let before = manager.tool_focus(id);
        match target {
            None => {
                if before.is_some() {
                    manager.tool_proximity_out(id);
                    self.sync_pads(manager, tablet);
                }
                None
            }
            Some(target) => {
                if before != Some(target.surface) {
                    manager.tool_proximity_in(id, tablet, target.surface);
                    self.sync_pads(manager, tablet);
                }
                Some(target)
            }
        }
    }

    /// The axes this tool has, and a frame. A client told of a pressure a
    /// pen cannot measure would trust it.
    fn send_axes(
        &self,
        manager: &TabletManager<'_>,
        backend_tool: u64,
        axes: &TabletAxes,
        target: Option<TabletTarget>,
        entered: bool,
        time_ms: u32,
    ) {
        let (Some(tool), Some(target)) = (self.tools.get(&backend_tool), target) else {
            return;
        };
        if manager.tool_focus(tool.id).is_none() {
            return;
        }
        let (id, caps) = (tool.id, tool.caps);
        if axes.has_position() || entered {
            manager.tool_motion(id, target.x, target.y);
        }
        if caps.pressure && axes.has_pressure() {
            manager.tool_pressure(id, axes.pressure);
        }
        if caps.distance && axes.has_distance() {
            manager.tool_distance(id, axes.distance);
        }
        if caps.tilt && axes.has_tilt() {
            manager.tool_tilt(id, axes.tilt_x, axes.tilt_y);
        }
        if caps.rotation && axes.has_rotation() {
            manager.tool_rotation(id, axes.rotation);
        }
        if caps.slider && axes.has_slider() {
            manager.tool_slider(id, axes.slider);
        }
        if caps.wheel && axes.has_wheel() {
            manager.tool_wheel(id, axes.wheel_degrees, axes.wheel_clicks);
        }
        manager.tool_frame(id, time_ms);
    }
}
