//! Shell primitives: OutputLayout, immediate-mode SceneItem, hit-testing, and
//! software CpuCompositor, plus the two that close the wiring every compositor
//! would otherwise write: [`ScreenManager`] and [`InputRouter`].

#[cfg(feature = "shell")]
use std::cell::RefCell;

use crate::backend::Output;
#[cfg(feature = "shell")]
use crate::backend::{AutoBackend, InputEvent};
#[cfg(feature = "shell")]
use crate::core::{Display, Holds};
use crate::core::{Error, ErrorKind};
use crate::ffi;
#[cfg(feature = "shell")]
use crate::ffi_shell;
#[cfg(feature = "shell")]
use crate::gpu::{Cursor, SceneRenderer};
#[cfg(feature = "shell")]
use crate::protocol::{Layer, LayerShell, LayerSurface, Seat, XdgShell};
use crate::protocol::{SurfaceId, Toplevel};
use crate::util::{Color, Rect};
#[cfg(feature = "shell")]
use crate::util::{DrmFourcc, Keysym};

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
            Err(Error::new(
                ErrorKind::Shell,
                "failed to create output layout",
            ))
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

    /// The first output placed, where a window with no better idea goes; empty
    /// when there are none.
    pub fn primary_box(&self) -> Rect {
        self.raw.primary_box()
    }

    /// The first output's scale: what a window with no better idea should
    /// render at, and 1 when there are none.
    pub fn primary_scale(&self) -> i32 {
        self.raw.primary_scale()
    }

    /// The box around every output, empty when there are none: the bounds a
    /// relative pointer is kept in.
    pub fn bounds(&self) -> Rect {
        self.raw.bounds()
    }
}

/// What a [`SceneItem`] paints.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SceneKind {
    /// A client surface tree, its root at `surface_origin`, clipped to `rect`.
    Surface = 0,
    /// A solid rectangle filling `rect`.
    Rect = 1,
    /// A ring `thickness` thick just inside `rect`: a window border.
    Border = 2,
    /// Compositor-owned pixels registered with `SceneRenderer::set_image`
    /// under `serial`.
    Image = 3,
    /// The last pixels of a surface that may be gone: [`SceneItem::retained`].
    Retained = 4,
}

impl SceneKind {
    #[cfg(feature = "shell")]
    fn from_raw(raw: u32) -> Self {
        match raw {
            0 => Self::Surface,
            1 => Self::Rect,
            2 => Self::Border,
            4 => Self::Retained,
            _ => Self::Image,
        }
    }
}

/// An entry in the immediate-mode scene list.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SceneItem {
    pub kind: SceneKind,
    pub rect: Rect,
    pub surface: SurfaceId,
    pub surface_origin: (i32, i32),
    pub color: Color,
    pub thickness: i32,
    pub serial: u64,
    pub tag: u64,
    pub accepts_input: bool,
    /// The [`Output::key`] of the one output to draw this on, or 0 for all of them.
    /// A window reaching past a shared edge stays whole this way, its rounded
    /// corners where they are, rather than being cut to fit and rounded again.
    pub only_on: u64,
    /// [`SceneKind::Retained`] only: the part of the surface to show, as
    /// `(x, y, width, height)` from 0 to 1.
    pub crop: (f32, f32, f32, f32),
    pub alpha: f32,
    pub corner_radius: f32,
    pub blur_enabled: bool,
    pub blur_xray: bool,
    /// How much of the blur shows. Separate from `alpha`: a window translucent
    /// by rule wants all of its blur, one fading in wants it to fade too.
    pub blur_opacity: f32,
    pub shadow_color: Color,
    pub shadow_feather: f32,
}

impl SceneItem {
    fn base(kind: SceneKind, rect: Rect) -> Self {
        Self {
            kind,
            rect,
            surface: SurfaceId::INVALID,
            surface_origin: (0, 0),
            color: Color::rgba(0.0, 0.0, 0.0, 0.0),
            thickness: 0,
            serial: 0,
            tag: 0,
            accepts_input: true,
            only_on: 0,
            crop: (0.0, 0.0, 1.0, 1.0),
            alpha: 1.0,
            corner_radius: 0.0,
            blur_enabled: false,
            blur_xray: false,
            blur_opacity: 1.0,
            shadow_color: Color::rgba(0.0, 0.0, 0.0, 0.0),
            shadow_feather: 0.0,
        }
    }

    pub fn surface(surface: SurfaceId, rect: Rect, origin_x: i32, origin_y: i32) -> Self {
        Self {
            surface,
            surface_origin: (origin_x, origin_y),
            ..Self::base(SceneKind::Surface, rect)
        }
    }

    /// `toplevel` with its window geometry at (`x`, `y`): a client's shadow sits
    /// outside its geometry, so the geometry, not the buffer, is what lands
    /// there. A window that has not set a geometry is placed by its surface.
    ///
    /// Tagged with its surface, which is what a hit on it, on its subsurfaces or
    /// on the popups [`PopupTree::append_to`] puts above it hands back:
    /// `SurfaceId(hit.tag)`.
    pub fn toplevel(toplevel: &Toplevel<'_>, x: i32, y: i32) -> Self {
        let s = toplevel.surface();
        let g = toplevel.geometry();
        let framed = g.width > 0 && g.height > 0;
        let (w, h) = if framed {
            (g.width, g.height)
        } else {
            (s.surface_width(), s.surface_height())
        };
        let (ox, oy) = if framed { (g.x, g.y) } else { (0, 0) };
        Self::surface(s, Rect::new(x, y, w, h), x - ox, y - oy).with_tag(s.0)
    }

    pub fn rect(rect: Rect, color: Color) -> Self {
        Self {
            color,
            ..Self::base(SceneKind::Rect, rect)
        }
    }

    pub fn border(rect: Rect, thickness: i32, color: Color) -> Self {
        Self {
            color,
            thickness,
            ..Self::base(SceneKind::Border, rect)
        }
    }

    /// The last pixels of `surface`, which may be gone: to fade a closed window out
    /// with. Stops being drawable once a frame goes by without one of these. See
    /// [`SceneRenderer::has_retained`](crate::gpu::SceneRenderer::has_retained).
    pub fn retained(surface: SurfaceId, rect: Rect, crop: (f32, f32, f32, f32)) -> Self {
        Self {
            surface,
            crop,
            accepts_input: false,
            ..Self::base(SceneKind::Retained, rect)
        }
    }

    pub fn image(rect: Rect, serial: u64) -> Self {
        Self {
            serial,
            ..Self::base(SceneKind::Image, rect)
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

    pub fn with_blur_opacity(mut self, opacity: f32) -> Self {
        self.blur_opacity = opacity;
        self
    }

    pub fn with_shadow(mut self, color: Color, feather: f32) -> Self {
        self.shadow_color = color;
        self.shadow_feather = feather;
        self
    }

    pub fn with_only_on(mut self, output: u64) -> Self {
        self.only_on = output;
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

    #[cfg(feature = "shell")]
    pub(crate) fn from_raw(raw: &ffi_shell::SceneItemRaw) -> Self {
        Self {
            kind: SceneKind::from_raw(raw.kind),
            rect: Rect::new(raw.x, raw.y, raw.width, raw.height),
            surface: SurfaceId(raw.surface_id),
            surface_origin: (raw.surface_x, raw.surface_y),
            color: Color::rgba(raw.r, raw.g, raw.b, raw.a),
            thickness: raw.thickness,
            serial: raw.serial,
            tag: raw.tag,
            accepts_input: raw.accepts_input,
            only_on: raw.only_on,
            crop: (raw.crop_x, raw.crop_y, raw.crop_width, raw.crop_height),
            alpha: raw.alpha,
            corner_radius: raw.corner_radius,
            blur_enabled: raw.blur_enabled,
            blur_xray: raw.blur_xray,
            blur_opacity: raw.blur_opacity,
            shadow_color: Color::rgba(raw.shadow_r, raw.shadow_g, raw.shadow_b, raw.shadow_a),
            shadow_feather: raw.shadow_feather,
        }
    }

    #[cfg(feature = "shell")]
    pub(crate) fn to_raw(&self) -> ffi_shell::SceneItemRaw {
        ffi_shell::SceneItemRaw {
            kind: self.kind as u32,
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
            only_on: self.only_on,
            crop_x: self.crop.0,
            crop_y: self.crop.1,
            crop_width: self.crop.2,
            crop_height: self.crop.3,
            alpha: self.alpha,
            corner_radius: self.corner_radius,
            blur_enabled: self.blur_enabled,
            blur_xray: self.blur_xray,
            blur_opacity: self.blur_opacity,
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
#[cfg(feature = "shell")]
pub fn scene_hit_test(items: &[SceneItem], x: f64, y: f64) -> SceneHit {
    let raw_items: Vec<ffi_shell::SceneItemRaw> = items.iter().map(SceneItem::to_raw).collect();
    SceneHit::from_raw(ffi_shell::raw_scene_hit_test(&raw_items, x, y))
}

impl SceneHit {
    #[cfg(feature = "shell")]
    fn from_raw(raw: ffi_shell::SceneHitRaw) -> Self {
        Self {
            hit: raw.hit,
            tag: raw.tag,
            surface: SurfaceId::from_raw(raw.surface_id),
            sx: raw.sx,
            sy: raw.sy,
        }
    }
}

/// Tell `seat` the pointer is at (`x`, `y`) in `items`. With a button held
/// (an implicit grab) every motion goes to the surface that took the press, in
/// its own coordinates, wherever the pointer is; otherwise the surface under
/// the point gets focus, or focus is cleared over nothing. Answers the hit
/// under the point either way.
#[cfg(feature = "shell")]
pub fn scene_route_pointer(
    seat: &crate::protocol::Seat<'_>,
    items: &[SceneItem],
    x: f64,
    y: f64,
) -> SceneHit {
    let raw_items: Vec<ffi_shell::SceneItemRaw> = items.iter().map(SceneItem::to_raw).collect();
    SceneHit::from_raw(ffi_shell::raw_scene_route_pointer(
        &seat.raw, &raw_items, x, y,
    ))
}

/// Where `surface`'s own origin is drawn in `items`: a root's item origin, or
/// that plus a subsurface's offset in its tree. `None` when it is not drawn.
#[cfg(feature = "shell")]
pub fn scene_surface_origin(items: &[SceneItem], surface: SurfaceId) -> Option<(f64, f64)> {
    let raw_items: Vec<ffi_shell::SceneItemRaw> = items.iter().map(SceneItem::to_raw).collect();
    let (mut x, mut y) = (0.0, 0.0);
    ffi_shell::raw_scene_surface_origin(&raw_items, surface.0, &mut x, &mut y).then_some((x, y))
}

#[cfg(feature = "render")]
pub use crate::render::CpuCompositor;

/// Every popup an [`XdgShell`] creates, from creation to destruction: drawn
/// above its parent by [`append_to`](Self::append_to), dismissed by a click
/// outside it. It follows the shell by itself; a compositor only draws it, and
/// asks for a frame on the shell's popup events.
///
/// It needs no borrow of the shell: the C++ tree it wraps lets go of each
/// popup when the popup is destroyed, and of the shell's signal when either
/// side goes first.
#[cfg(feature = "shell")]
pub struct PopupTree {
    pub(crate) raw: cxx::UniquePtr<ffi_shell::PopupTreeShim>,
}

#[cfg(feature = "shell")]
impl PopupTree {
    pub fn new(shell: &XdgShell<'_>) -> Result<Self, Error> {
        let raw = ffi_shell::popup_tree_new(&shell.raw);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Shell, "failed to create popup tree"))
        } else {
            Ok(Self { raw })
        }
    }

    /// Re-parent `popup` onto `parent`: how a layer surface's popup, which has
    /// no xdg parent, is placed (`LayerSurfaceEvent::NewPopup`). False if
    /// `shell` has no such popup (any more).
    pub fn set_parent(&self, shell: &XdgShell<'_>, popup: SurfaceId, parent: SurfaceId) -> bool {
        self.raw.set_parent(&shell.raw, popup.0, parent.0)
    }

    /// Push the mapped popups onto `scene`, each above its parent's surface
    /// item and at its offset from it, carrying the parent's tag. Popups whose
    /// parent is not in `scene` are left out. Submenus nest.
    pub fn append_to(&self, scene: &mut Vec<SceneItem>) {
        let raw: Vec<ffi_shell::SceneItemRaw> = scene.iter().map(SceneItem::to_raw).collect();
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
        self.raw
            .pin_mut()
            .dismiss_grabs_outside(surface.map_or(0, |s| s.0))
    }
}

/// Layer surfaces per output: their layout, exclusive zones and the usable
/// area they leave, and their place in the scene. Outputs are named by an id
/// the compositor picks.
///
/// Like [`PopupTree`] it drops a destroyed layer surface by itself and needs
/// no borrow of the [`LayerShell`].
#[cfg(feature = "shell")]
pub struct LayerManager {
    raw: cxx::UniquePtr<ffi_shell::LayerManagerShim>,
}

#[cfg(feature = "shell")]
impl LayerManager {
    pub fn new() -> Result<Self, Error> {
        let raw = ffi_shell::layer_manager_new();
        if raw.is_null() {
            Err(Error::new(
                ErrorKind::Shell,
                "failed to create layer manager",
            ))
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
        scene.extend(
            self.raw
                .append_to(output, layer as u32)
                .iter()
                .map(SceneItem::from_raw),
        );
    }

    /// The mapped layer surface `surface` belongs to, if it may take keyboard
    /// focus.
    pub fn keyboard_target(&self, surface: SurfaceId) -> Option<SurfaceId> {
        match self.raw.keyboard_target(surface.0) {
            0 => None,
            id => Some(SurfaceId(id)),
        }
    }

    /// The surfaces on `output`, to tell them about a change to it.
    pub fn surfaces_on(&self, output: u32) -> Vec<SurfaceId> {
        let raw = self.raw.surfaces_on(output);
        raw.into_iter().map(SurfaceId).collect()
    }

    /// Close every surface on `output`: its monitor went away.
    pub fn close_output(&mut self, output: u32) {
        self.raw.pin_mut().close_output(output);
    }
}

/// What a [`ScreenManager`] does with an output on arrival.
#[cfg(feature = "shell")]
#[derive(Debug, Clone, PartialEq)]
pub struct ScreenOptions {
    /// What shows where the scene does not reach.
    pub background: Color,
    /// Place it right of everything already placed. Off for a compositor that
    /// arranges outputs itself, from [`Event::OutputAdded`](crate::Event::OutputAdded).
    pub join_layout: bool,
    /// Open a `wl_output` that follows the output and the layout.
    pub announce: bool,
    /// Variable refresh on a monitor that has it: the panel follows the frames,
    /// so an idle desktop idles it and a game runs as fast as it renders.
    pub variable_refresh: bool,
    /// The `wl_output`s are called "<name>-1", "<name>-2", ...; a number is never
    /// used twice, so a replugged monitor cannot collide with another.
    pub name: String,
    pub format: DrmFourcc,
}

#[cfg(feature = "shell")]
impl Default for ScreenOptions {
    fn default() -> Self {
        Self {
            background: Color::rgb(0.0, 0.0, 0.0),
            join_layout: true,
            announce: true,
            variable_refresh: false,
            name: "luminaria".into(),
            format: DrmFourcc::Xrgb8888,
        }
    }
}

/// Every output of a backend, for as long as it exists: render targets, its
/// place in the layout, its `wl_output`, and a frame drawn whenever it asks for
/// one. What is left to the compositor is what the screen shows.
///
/// C++ never calls Rust, so where C++ has a scene builder this is told the scene
/// ahead of time: [`set_scene`](Self::set_scene) after every dispatch. Everything
/// that needs the scene inside a dispatch — a frame, a click, the shell placing
/// a popup — reads the last list it was told, which is at most one dispatch old
/// and made of generational ids, so a window gone since draws nothing rather than
/// the wrong thing.
///
/// Make it before [`AutoBackend::start`], which is when the outputs arrive. They
/// arrive as [`Event::OutputAdded`](crate::Event::OutputAdded) too, for a
/// compositor that wants to look at them; dropping those changes nothing. Listen
/// to [`Event::LayoutChanged`](crate::Event::LayoutChanged) to lay windows out.
///
/// The renderer, the layout and the display stay borrowed for as long as it lives:
///
/// ```compile_fail,E0505
/// # use luminaria::{backend::{AutoBackend, BackendOptions}, core::Display,
/// #                 gpu::{SceneRenderer, VulkanOptions},
/// #                 shell::{OutputLayout, ScreenManager, ScreenOptions}};
/// let display = Display::new()?;
/// let backend = AutoBackend::new(&display, &BackendOptions::default())?;
/// let renderer = SceneRenderer::new(&VulkanOptions::default())?;
/// let layout = OutputLayout::new()?;
/// let screens = ScreenManager::new(&display, &backend, &renderer, &layout,
///                                  &ScreenOptions::default())?;
/// drop(renderer); // error[E0505]: cannot move out of `renderer` because it is borrowed
/// # drop(screens);
/// # Ok::<(), luminaria::core::Error>(())
/// ```
#[cfg(feature = "shell")]
pub struct ScreenManager<'a> {
    pub(crate) raw: cxx::UniquePtr<ffi_shell::ScreenManagerShim>,
    told: RefCell<Vec<SceneItem>>,
    _borrows: Holds<'a>,
}

#[cfg(feature = "shell")]
impl<'a> ScreenManager<'a> {
    /// The backend is only read here: its outputs are followed by their own
    /// signals from then on.
    pub fn new(
        display: &'a Display,
        backend: &AutoBackend<'_>,
        renderer: &'a SceneRenderer,
        layout: &'a OutputLayout,
        options: &ScreenOptions,
    ) -> Result<Self, Error> {
        let raw_options = ffi_shell::ScreenOptionsRaw {
            r: options.background.r,
            g: options.background.g,
            b: options.background.b,
            a: options.background.a,
            join_layout: options.join_layout,
            announce: options.announce,
            variable_refresh: options.variable_refresh,
            name: options.name.clone(),
            drm_format: options.format as u32,
        };
        let mut err = String::new();
        let raw = ffi_shell::screen_manager_new(
            &display.raw,
            &backend.raw,
            &renderer.raw,
            &layout.raw,
            &raw_options,
            &mut err,
        );
        if raw.is_null() {
            Err(Error::new(ErrorKind::Shell, err))
        } else {
            Ok(Self {
                raw,
                told: RefCell::new(Vec::new()),
                _borrows: Holds::new(),
            })
        }
    }

    /// Redraw when `popups` change, and keep a popup on the output its parent is
    /// on, looked up in the scene when the shell asks. Instead of
    /// [`XdgShell::set_popup_constraint_areas`], which stops being consulted.
    pub fn follow_popups(&self, popups: &'a PopupTree) {
        self.raw.follow_popups(&popups.raw);
    }

    /// Let clients capture every output, past and future: `grim`, `wf-recorder`, a
    /// portal. They get what was last drawn.
    pub fn follow_capture(&self, capture: &'a crate::render::ScreencopyManager<'_>) {
        self.raw.follow_capture(&capture.raw);
    }

    /// Where captures get the pointer: cursor sessions, and a frame that asks for
    /// the pointer when a KMS plane is showing it. After
    /// [`follow_capture`](Self::follow_capture).
    pub fn follow_cursor(&self, cursor: &'a crate::gpu::Cursor<'_>) {
        self.raw.follow_cursor(&cursor.raw);
    }

    /// The list to draw, in layout coordinates: one list for every output, each
    /// showing the part the layout puts it on. Call it after every dispatch. A
    /// list that differs from the last one asks every output for a frame and
    /// picks the surface under the pointer again; the same list costs a compare.
    pub fn set_scene(&self, items: &[SceneItem]) {
        let mut told = self.told.borrow_mut();
        if told.as_slice() == items {
            return;
        }
        told.clear();
        told.extend_from_slice(items);
        let raw: Vec<ffi_shell::SceneItemRaw> = items.iter().map(SceneItem::to_raw).collect();
        self.raw.set_scene(&raw);
    }

    /// What shows where the scene does not reach, from the next frame on, and
    /// what a blur over a bare spot samples. The same colour again costs nothing,
    /// so a config reload can just call this.
    pub fn set_background(&self, background: Color) {
        let Color { r, g, b, a } = background;
        self.raw.set_background(r, g, b, a);
    }

    /// Which of `outputs` the client asked `surface` to be on: `None` when it left
    /// that to the compositor ([`LayerSurface::has_output`] tells that apart), or
    /// named one that is already gone. A wallpaper client makes one surface per
    /// output, so the focused monitor is the wrong answer.
    pub fn output_of<'o>(
        &self,
        surface: &LayerSurface<'_>,
        outputs: &'o [Output],
    ) -> Option<&'o Output> {
        let (shell, id) = (surface.shell, surface.surface.0);
        outputs
            .iter()
            .find(|output| self.raw.output_is(shell, id, &output.raw))
    }

    /// What `output`'s `wl_output` is called: "DP-1", or a made-up name for an
    /// output that isn't a connector.
    pub fn output_name(&self, output: &Output) -> String {
        self.raw.output_name(&output.raw)
    }

    /// The identity protocol objects report for `output`'s `wl_output`, 0 while
    /// it has none: what [`crate::desktop::WorkspaceManager::group_remove_output`]
    /// and [`Self::output_with`] take.
    pub fn output_identity(&self, output: &Output) -> u64 {
        self.raw.output_identity(&output.raw)
    }

    /// Which of `outputs` a protocol object names, by the identity it reports, as
    /// [`LockSurface::output_identity`](crate::desktop::LockSurface::output_identity)
    /// does. An output has one for as long as it has a `wl_output`.
    pub fn output_with<'o>(&self, identity: u64, outputs: &'o [Output]) -> Option<&'o Output> {
        if identity == 0 {
            return None;
        }
        outputs
            .iter()
            .find(|output| self.raw.output_identity(&output.raw) == identity)
    }

    /// Where `surface` is in the scene.
    pub fn surface_box(&self, surface: SurfaceId) -> Option<Rect> {
        let mut out = Rect::default();
        self.raw.surface_box(surface.0, &mut out).then_some(out)
    }

    /// Something no client reports changed and the scene does not show: every
    /// output is asked for a frame, and the pointer picks again.
    pub fn invalidate(&self) {
        self.raw.invalidate();
    }

    /// How many outputs are being managed.
    pub fn len(&self) -> usize {
        self.raw.size()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// Input, given what a key or a click means: motion to the cursor and the scene,
/// the keyboard through the bindings and past the VT switch, a click outside a
/// menu closing it, a press giving its window the keyboard, the surface under the
/// pointer picked again whenever the scene changes. The C++ `DefaultInputRouter`.
///
/// It runs inside dispatch, against the list [`ScreenManager::set_scene`] was
/// last told, so bindings are made ahead of time ([`bind`](Self::bind)) and what
/// they mean arrives after the fact: [`Event::KeyBinding`](crate::Event::KeyBinding)
/// and [`Event::Button`](crate::Event::Button).
#[cfg(feature = "shell")]
pub struct InputRouter<'a> {
    raw: cxx::UniquePtr<ffi_shell::InputRouterShim>,
    _borrows: Holds<'a>,
}

#[cfg(feature = "shell")]
impl<'a> InputRouter<'a> {
    pub fn new(
        seat: &'a Seat<'_>,
        cursor: &'a Cursor<'_>,
        screens: &'a ScreenManager<'_>,
        shell: &'a XdgShell<'_>,
        popups: &'a PopupTree,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi_shell::input_router_new(
            &seat.raw,
            &cursor.raw,
            &screens.raw,
            &shell.raw,
            &popups.raw,
            &mut err,
        );
        if raw.is_null() {
            Err(Error::new(ErrorKind::Shell, err))
        } else {
            Ok(Self {
                raw,
                _borrows: Holds::new(),
            })
        }
    }

    /// Take input from `backend`, have the seat follow its keymap and devices,
    /// and on a session: Ctrl+Alt+Fn switches VTs, keys held are released when the
    /// VT goes away, and every output repaints when it comes back.
    pub fn attach(&self, backend: &'a AutoBackend<'_>) {
        self.raw.attach(&backend.raw);
    }

    /// `mods` ([`Modifiers::LOGO`](crate::backend::Modifiers::LOGO) and friends,
    /// exactly those held) with `keysym`: the client never sees the key, and
    /// [`Event::KeyBinding`](crate::Event::KeyBinding)`(id)` arrives instead.
    /// Caps Lock changes nothing: `keysym` is compared in lower case.
    pub fn bind(&self, mods: u32, keysym: Keysym, id: u32) {
        self.raw.bind(mods, keysym.raw(), id);
    }

    /// A press on a window gives it the keyboard and the activated look; a press
    /// in a popup, which is its parent's business, changes nothing. On by
    /// default. Off for a compositor that keeps focus for itself.
    pub fn set_click_to_focus(&self, enabled: bool) {
        self.raw.set_click_to_focus(enabled);
    }

    /// Pick the surface under the pointer again.
    pub fn update_pointer(&self) {
        self.raw.update_pointer();
    }

    /// One event, as [`attach`](Self::attach) would deliver it, for input that
    /// does not come from the backend. Absolute motion is taken on the layout's
    /// first output; events no default exists for (touch, tablets) are ignored.
    pub fn route(&self, event: &InputEvent) {
        match *event {
            InputEvent::Key {
                key,
                pressed,
                keysym,
                modifiers: m,
            } => self.raw.route_key(
                key,
                pressed,
                keysym.raw(),
                m.depressed,
                m.latched,
                m.locked,
                m.group,
            ),
            InputEvent::Modifiers(m) => {
                self.raw
                    .route_modifiers(m.depressed, m.latched, m.locked, m.group);
            }
            InputEvent::Motion { dx, dy } => self.raw.route_motion(dx, dy),
            InputEvent::MotionAbsolute { x, y, .. } => self.raw.route_motion_absolute(x, y),
            InputEvent::PointerLeave => self.raw.route_leave(),
            InputEvent::Button { button, pressed } => self.raw.route_button(button, pressed),
            InputEvent::Axis(a) => self.raw.route_axis(
                a.dx,
                a.dy,
                a.dx_steps,
                a.dy_steps,
                a.stop_horizontal,
                a.stop_vertical,
            ),
            _ => {}
        }
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
