//! Shell primitives: OutputLayout, immediate-mode SceneItem, hit-testing, and software CpuCompositor.

use crate::backend::Output;
use crate::core::Error;
use crate::ffi;
use crate::protocol::{Layer, LayerShell, SurfaceId, XdgShell};
use crate::util::{Color, Rect};

/// Manages arrangement of multiple outputs in global logical coordinates.
///
/// [`Cursor`](crate::gpu::Cursor) and
/// [`OutputManager`](crate::desktop::OutputManager) keep it borrowed while they
/// live, so its methods take `&self`. It holds the outputs added to it by
/// pointer, but needs no borrow of them: each one leaves the layout by itself
/// when it is destroyed.
pub struct OutputLayout {
    pub(crate) raw: cxx::UniquePtr<ffi::OutputLayoutShim>,
}

impl OutputLayout {
    pub fn new() -> Result<Self, Error> {
        let raw = ffi::output_layout_new();
        if raw.is_null() {
            Err(Error("failed to create output layout".into()))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn add(&self, output: &mut Output, x: i32, y: i32) {
        self.raw.add(output.raw.pin_mut(), x, y);
    }

    /// Place `output` to the right of everything already placed.
    pub fn add_auto(&self, output: &mut Output) {
        self.raw.add_auto(output.raw.pin_mut());
    }

    /// An output that is destroyed (a monitor unplugged) leaves the layout by
    /// itself, before Rust hears of it; removing it by hand is for policy.
    pub fn remove(&self, output: &mut Output) {
        self.raw.remove(output.raw.pin_mut());
    }

    pub fn output_box(&self, output: &Output) -> Rect {
        self.raw.output_box(&output.raw)
    }

    /// The box around every output, empty when there are none: the bounds a
    /// relative pointer is kept in.
    pub fn bounds(&self) -> Rect {
        self.raw.bounds()
    }
}

/// An entry in the immediate-mode scene list.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SceneItem {
    pub kind: u32, // 0 = surface, 1 = rect, 2 = border, 3 = image
    pub rect: Rect,
    pub surface: SurfaceId,
    pub surface_origin: (i32, i32),
    pub color: Color,
    pub thickness: i32,
    pub serial: u64,
    pub tag: u64,
    pub accepts_input: bool,
    pub alpha: f32,
    pub corner_radius: f32,
    pub blur_enabled: bool,
    pub blur_xray: bool,
    pub shadow_color: Color,
    pub shadow_feather: f32,
}

impl SceneItem {
    pub fn surface(surface: SurfaceId, rect: Rect, origin_x: i32, origin_y: i32) -> Self {
        Self {
            kind: 0,
            rect,
            surface,
            surface_origin: (origin_x, origin_y),
            color: Color::rgba(0.0, 0.0, 0.0, 0.0),
            thickness: 0,
            serial: 0,
            tag: 0,
            accepts_input: true,
            alpha: 1.0,
            corner_radius: 0.0,
            blur_enabled: false,
            blur_xray: false,
            shadow_color: Color::rgba(0.0, 0.0, 0.0, 0.0),
            shadow_feather: 0.0,
        }
    }

    pub fn rect(rect: Rect, color: Color) -> Self {
        Self {
            kind: 1,
            rect,
            surface: SurfaceId::INVALID,
            surface_origin: (0, 0),
            color,
            thickness: 0,
            serial: 0,
            tag: 0,
            accepts_input: true,
            alpha: 1.0,
            corner_radius: 0.0,
            blur_enabled: false,
            blur_xray: false,
            shadow_color: Color::rgba(0.0, 0.0, 0.0, 0.0),
            shadow_feather: 0.0,
        }
    }

    pub fn border(rect: Rect, thickness: i32, color: Color) -> Self {
        Self {
            kind: 2,
            rect,
            surface: SurfaceId::INVALID,
            surface_origin: (0, 0),
            color,
            thickness,
            serial: 0,
            tag: 0,
            accepts_input: true,
            alpha: 1.0,
            corner_radius: 0.0,
            blur_enabled: false,
            blur_xray: false,
            shadow_color: Color::rgba(0.0, 0.0, 0.0, 0.0),
            shadow_feather: 0.0,
        }
    }

    pub fn image(rect: Rect, serial: u64) -> Self {
        Self {
            kind: 3,
            rect,
            surface: SurfaceId::INVALID,
            surface_origin: (0, 0),
            color: Color::rgba(0.0, 0.0, 0.0, 0.0),
            thickness: 0,
            serial,
            tag: 0,
            accepts_input: true,
            alpha: 1.0,
            corner_radius: 0.0,
            blur_enabled: false,
            blur_xray: false,
            shadow_color: Color::rgba(0.0, 0.0, 0.0, 0.0),
            shadow_feather: 0.0,
        }
    }

    pub fn with_alpha(mut self, alpha: f32) -> Self {
        self.alpha = alpha;
        self
    }

    pub fn with_corner_radius(mut self, radius: f32) -> Self {
        self.corner_radius = radius;
        self
    }

    pub fn with_blur(mut self, xray: bool) -> Self {
        self.blur_enabled = true;
        self.blur_xray = xray;
        self
    }

    pub fn with_shadow(mut self, color: Color, feather: f32) -> Self {
        self.shadow_color = color;
        self.shadow_feather = feather;
        self
    }

    pub fn with_tag(mut self, tag: u64) -> Self {
        self.tag = tag;
        self
    }

    pub fn with_accepts_input(mut self, accepts: bool) -> Self {
        self.accepts_input = accepts;
        self
    }

    pub(crate) fn from_raw(raw: &ffi::SceneItemRaw) -> Self {
        Self {
            kind: raw.kind,
            rect: Rect::new(raw.x, raw.y, raw.width, raw.height),
            surface: SurfaceId(raw.surface_id),
            surface_origin: (raw.surface_x, raw.surface_y),
            color: Color::rgba(raw.r, raw.g, raw.b, raw.a),
            thickness: raw.thickness,
            serial: raw.serial,
            tag: raw.tag,
            accepts_input: raw.accepts_input,
            alpha: raw.alpha,
            corner_radius: raw.corner_radius,
            blur_enabled: raw.blur_enabled,
            blur_xray: raw.blur_xray,
            shadow_color: Color::rgba(raw.shadow_r, raw.shadow_g, raw.shadow_b, raw.shadow_a),
            shadow_feather: raw.shadow_feather,
        }
    }

    pub(crate) fn to_raw(&self) -> ffi::SceneItemRaw {
        ffi::SceneItemRaw {
            kind: self.kind,
            x: self.rect.x,
            y: self.rect.y,
            width: self.rect.width,
            height: self.rect.height,
            surface_id: self.surface.0,
            surface_x: self.surface_origin.0,
            surface_y: self.surface_origin.1,
            r: self.color.r,
            g: self.color.g,
            b: self.color.b,
            a: self.color.a,
            thickness: self.thickness,
            serial: self.serial,
            tag: self.tag,
            accepts_input: self.accepts_input,
            alpha: self.alpha,
            corner_radius: self.corner_radius,
            blur_enabled: self.blur_enabled,
            blur_xray: self.blur_xray,
            shadow_r: self.shadow_color.r,
            shadow_g: self.shadow_color.g,
            shadow_b: self.shadow_color.b,
            shadow_a: self.shadow_color.a,
            shadow_feather: self.shadow_feather,
        }
    }
}

/// The result of hit-testing against a scene.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SceneHit {
    pub hit: bool,
    pub tag: u64,
    pub surface: Option<SurfaceId>,
    pub sx: f64,
    pub sy: f64,
}

/// Hit-test against `items` (front-to-back, reverse of paint order) at global coordinates (`x`, `y`).
pub fn scene_hit_test(items: &[SceneItem], x: f64, y: f64) -> SceneHit {
    let raw_items: Vec<ffi::SceneItemRaw> = items.iter().map(SceneItem::to_raw).collect();
    let raw_hit = ffi::raw_scene_hit_test(&raw_items, x, y);
    SceneHit {
        hit: raw_hit.hit,
        tag: raw_hit.tag,
        surface: if raw_hit.surface_id != 0 {
            Some(SurfaceId(raw_hit.surface_id))
        } else {
            None
        },
        sx: raw_hit.sx,
        sy: raw_hit.sy,
    }
}

/// Software CPU compositor for nested and headless targets.
pub struct CpuCompositor {
    raw: cxx::UniquePtr<ffi::CpuCompositorShim>,
}

impl CpuCompositor {
    pub fn new() -> Result<Self, Error> {
        let raw = ffi::cpu_compositor_new();
        if raw.is_null() {
            Err(Error("failed to create cpu compositor".into()))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn composite_rect(&mut self, rect: Rect, color: Color) {
        self.raw.pin_mut().composite_rect(
            rect.x,
            rect.y,
            rect.width,
            rect.height,
            color.r,
            color.g,
            color.b,
            color.a,
        );
    }

    pub fn finish(&mut self, width: i32, height: i32, background: Color) -> Vec<u8> {
        self.raw.pin_mut().finish(
            width,
            height,
            background.r,
            background.g,
            background.b,
            background.a,
        )
    }
}

/// The mapped popups and where they go: above their parents in the scene,
/// dismissed by a click outside them.
///
/// It holds each popup it was given until the popup is destroyed, and lets go
/// of it by itself then; [`remove`](Self::remove) is for policy. It needs no
/// borrow of the [`XdgShell`]: the shell is only asked, in [`add`](Self::add),
/// which popup an id names.
pub struct PopupTree {
    raw: cxx::UniquePtr<ffi::PopupTreeShim>,
}

impl PopupTree {
    pub fn new() -> Result<Self, Error> {
        let raw = ffi::popup_tree_new();
        if raw.is_null() {
            Err(Error("failed to create popup tree".into()))
        } else {
            Ok(Self { raw })
        }
    }

    /// Track `popup`, drawn relative to its xdg parent. False if `shell` has
    /// no such popup (any more).
    pub fn add(&mut self, shell: &XdgShell<'_>, popup: SurfaceId) -> bool {
        self.raw.pin_mut().add(&shell.raw, popup.0, 0)
    }

    /// Track `popup` (or re-parent it) as a child of `parent`: how a layer
    /// surface's popup, which has no xdg parent, is placed.
    pub fn add_with_parent(&mut self, shell: &XdgShell<'_>, popup: SurfaceId, parent: SurfaceId) -> bool {
        self.raw.pin_mut().add(&shell.raw, popup.0, parent.0)
    }

    /// Only mapped popups are drawn and hit.
    pub fn set_mapped(&mut self, popup: SurfaceId, mapped: bool) {
        self.raw.pin_mut().set_mapped(popup.0, mapped);
    }

    pub fn remove(&mut self, popup: SurfaceId) {
        self.raw.pin_mut().remove(popup.0);
    }

    /// Push the mapped popups onto `scene`, each above its parent's surface
    /// item and at its offset from it, carrying the parent's tag. Popups whose
    /// parent is not in `scene` are left out. Submenus nest.
    pub fn append_to(&self, scene: &mut Vec<SceneItem>) {
        let raw: Vec<ffi::SceneItemRaw> = scene.iter().map(SceneItem::to_raw).collect();
        scene.extend(self.raw.append_to(&raw).iter().map(SceneItem::from_raw));
    }

    /// Whether `surface` is (part of) a mapped popup in the tree.
    pub fn contains(&self, surface: SurfaceId) -> bool {
        self.raw.contains(surface.0)
    }

    /// The topmost mapped popup holding a grab.
    pub fn active_grab(&self) -> Option<SurfaceId> {
        match self.raw.active_grab() {
            0 => None,
            id => Some(SurfaceId(id)),
        }
    }

    /// A click landed on `surface` (or on nothing): if that is outside every
    /// popup, dismiss the grabbing ones. True when one was dismissed, which
    /// is when the click should not reach a client.
    pub fn dismiss_grabs_outside(&mut self, surface: Option<SurfaceId>) -> bool {
        self.raw.pin_mut().dismiss_grabs_outside(surface.map_or(0, |s| s.0))
    }
}

/// Layer surfaces per output: their layout, exclusive zones and the usable
/// area they leave, and their place in the scene. Outputs are named by an id
/// the compositor picks.
///
/// Like [`PopupTree`] it drops a destroyed layer surface by itself and needs
/// no borrow of the [`LayerShell`].
pub struct LayerManager {
    raw: cxx::UniquePtr<ffi::LayerManagerShim>,
}

impl LayerManager {
    pub fn new() -> Result<Self, Error> {
        let raw = ffi::layer_manager_new();
        if raw.is_null() {
            Err(Error("failed to create layer manager".into()))
        } else {
            Ok(Self { raw })
        }
    }

    /// Put `surface` on output `output`. False if `shell` has no such layer
    /// surface (any more).
    pub fn add(&mut self, shell: &LayerShell<'_>, surface: SurfaceId, output: u32) -> bool {
        self.raw.pin_mut().add(&shell.raw, surface.0, output)
    }

    pub fn remove(&mut self, surface: SurfaceId) {
        self.raw.pin_mut().remove(surface.0);
    }

    /// Only mapped surfaces take space and are drawn.
    pub fn set_mapped(&mut self, surface: SurfaceId, mapped: bool) {
        self.raw.pin_mut().set_mapped(surface.0, mapped);
    }

    /// Configure every surface on `output`, whose whole box is `full`, and
    /// return what is left of `usable` after their exclusive zones: hand it
    /// to [`XdgShell::set_bounds`] and lay windows out in it.
    pub fn arrange(&mut self, output: u32, full: Rect, usable: Rect) -> Rect {
        self.raw.pin_mut().arrange(output, full, usable)
    }

    /// Push `output`'s mapped surfaces of `layer` onto `scene`, where the last
    /// [`arrange`](Self::arrange) put them.
    pub fn append_to(&self, scene: &mut Vec<SceneItem>, output: u32, layer: Layer) {
        scene.extend(self.raw.append_to(output, layer as u32).iter().map(SceneItem::from_raw));
    }

    /// The mapped layer surface `surface` belongs to, if it may take keyboard
    /// focus.
    pub fn keyboard_target(&self, surface: SurfaceId) -> Option<SurfaceId> {
        match self.raw.keyboard_target(surface.0) {
            0 => None,
            id => Some(SurfaceId(id)),
        }
    }

    /// Close every surface on `output`: its monitor went away.
    pub fn close_output(&mut self, output: u32) {
        self.raw.pin_mut().close_output(output);
    }
}

/// Pointer position and boundary clamping tracker.
#[derive(Debug, Clone, Copy, Default)]
pub struct Pointer {
    x: f64,
    y: f64,
    present: bool,
}

impl Pointer {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn is_present(&self) -> bool {
        self.present
    }

    pub fn x(&self) -> f64 {
        self.x
    }

    pub fn y(&self) -> f64 {
        self.y
    }

    pub fn move_to(&mut self, x: f64, y: f64, bounds: Rect) -> bool {
        let last = |origin: i32, extent: i32| (origin + extent - 1).max(origin) as f64;
        let clamped_x = x.clamp(bounds.x as f64, last(bounds.x, bounds.width));
        let clamped_y = y.clamp(bounds.y as f64, last(bounds.y, bounds.height));
        let moved = !self.present || clamped_x != self.x || clamped_y != self.y;
        self.x = clamped_x;
        self.y = clamped_y;
        self.present = true;
        moved
    }

    pub fn move_by(&mut self, dx: f64, dy: f64, bounds: Rect) -> bool {
        let base_x = if self.present {
            self.x
        } else {
            bounds.x as f64 + bounds.width as f64 / 2.0
        };
        let base_y = if self.present {
            self.y
        } else {
            bounds.y as f64 + bounds.height as f64 / 2.0
        };
        self.move_to(base_x + dx, base_y + dy, bounds)
    }

    pub fn leave(&mut self) -> bool {
        std::mem::replace(&mut self.present, false)
    }
}
