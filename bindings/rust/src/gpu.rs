//! GPU rendering, Vulkan integration, direct scanout, and hardware frame ledgers.
//!
//! Re-exports types from [`crate::render`] and the shell pipeline.

use std::pin::Pin;

use crate::backend::Output;
use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi_shell as ffi;
use crate::protocol::{Presentation, Seat, SurfaceId};
use crate::shell::{OutputLayout, SceneItem};
use crate::util::{Color, DrmFourcc, Rect};

#[cfg(feature = "render")]
pub use crate::render::*;

/// Outcome of presenting a scene.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SceneOutcome {
    Composited = 0,
    Scanout = 1,
    Unchanged = 2,
    Cpu = 3,
}

impl SceneOutcome {
    fn from_result(code: i32, error: String) -> Result<Self, Error> {
        match code {
            0 => Ok(Self::Composited),
            1 => Ok(Self::Scanout),
            2 => Ok(Self::Unchanged),
            3 => Ok(Self::Cpu),
            _ => Err(Error::new(ErrorKind::Gpu, error)),
        }
    }
}

/// Unified scene presenter over Vulkan GPU and CPU fallback.
///
/// It keeps every output's render state itself, by output, and drops it when
/// the output is destroyed, so there is nothing per output for the compositor
/// to hold. [`Cursor`] and [`LinuxDmabuf`] keep it borrowed while they live,
/// so its methods take `&self`. It borrows nothing itself.
pub struct SceneRenderer {
    pub(crate) raw: cxx::UniquePtr<ffi::SceneRendererShim>,
}

impl SceneRenderer {
    pub fn new(options: &VulkanOptions) -> Result<Self, Error> {
        let mut err = String::new();
        let (has_drm, drm) = options.drm_device_raw();
        let raw = ffi::scene_renderer_new(has_drm, &drm, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn gpu_enabled(&self) -> bool {
        self.raw.gpu_enabled()
    }

    pub fn gpu_error(&self) -> String {
        self.raw.gpu_error()
    }

    /// Size this output's render targets for its current mode, in XRGB8888.
    /// Once per output: a later mode change reallocates by itself. A failure
    /// is not fatal — the output is drawn on the CPU instead — but is worth
    /// reporting.
    pub fn reset(&self, output: &mut Output) -> Result<(), Error> {
        self.reset_with_format(output, DrmFourcc::Xrgb8888)
    }

    /// [`reset`](Self::reset) in a format of the caller's choosing.
    pub fn reset_with_format(&self, output: &mut Output, format: DrmFourcc) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.reset(output.raw.pin_mut(), format as u32, &mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Gpu, err))
        }
    }

    /// Draw `items`, in `output`'s own logical coordinates, and present them.
    /// Call it on [`OutputEvent::Frame`](crate::backend::OutputEvent::Frame).
    pub fn present(&self, output: &mut Output, items: &[SceneItem], background: Color) -> Result<SceneOutcome, Error> {
        let raw_items: Vec<ffi::SceneItemRaw> = items.iter().map(SceneItem::to_raw).collect();
        let mut err = String::new();
        let (r, g, b, a) = (background.r, background.g, background.b, background.a);
        let res = self.raw.present(output.raw.pin_mut(), &raw_items, r, g, b, a, &mut err);
        SceneOutcome::from_result(res, err)
    }

    /// The same from a scene in `layout`'s coordinates: one list for every
    /// output, each showing the part `layout` puts it on.
    pub fn present_in(
        &self,
        layout: &OutputLayout,
        output: &mut Output,
        items: &[SceneItem],
        background: Color,
    ) -> Result<SceneOutcome, Error> {
        let raw_items: Vec<ffi::SceneItemRaw> = items.iter().map(SceneItem::to_raw).collect();
        let mut err = String::new();
        let (r, g, b, a) = (background.r, background.g, background.b, background.a);
        let res = self.raw.present_in(&layout.raw, output.raw.pin_mut(), &raw_items, r, g, b, a, &mut err);
        SceneOutcome::from_result(res, err)
    }

    /// Whether `output` is currently drawn on the GPU.
    pub fn gpu_output(&self, output: &Output) -> bool {
        self.raw.gpu_output(&output.raw)
    }

    /// Where presentation feedback goes. The renderer usually outlives the
    /// global (it is declared before the `Display`), so there is no borrow
    /// here: a `Presentation` that drops first unhooks itself, and feedback
    /// simply stops.
    pub fn set_presentation(&self, presentation: Option<&Presentation<'_>>) {
        if let Some(pres) = presentation {
            self.raw.set_presentation(&pres.raw);
        } else {
            self.raw.clear_presentation();
        }
    }

    pub fn update_cursor(&self) {
        self.raw.update_cursor();
    }

    /// The GPU's raw Vulkan handles; `None` when this renderer fell back to the
    /// CPU. Same contract as [`VulkanRenderer::raw_handles`].
    #[cfg(feature = "raw-vulkan")]
    pub fn raw_vulkan_handles(&self) -> Option<RawVulkanHandles> {
        let mut h = ffi::VulkanHandlesRaw::default();
        self.raw.vulkan_handles(&mut h).then(|| h.into())
    }

    /// The last frame presented on `output`, as RGBA8.
    pub fn read_back(&self, output: &Output) -> Result<Vec<u8>, Error> {
        let mut err = String::new();
        let vec = self.raw.read_back(&output.raw, &mut err);
        if vec.is_empty() && !err.is_empty() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(vec)
        }
    }

    /// Create [`LinuxDmabuf`] backed by this renderer's GPU.
    pub fn create_linux_dmabuf<'a>(&'a self, display: &'a Display) -> Result<LinuxDmabuf<'a>, Error> {
        LinuxDmabuf::from_scene_renderer(display, self)
    }

    /// Register compositor-owned image pixels for a given serial, accepting
    /// tightly-packed RGBA `&[u8]` pixels (e.g. from the `image` crate).
    pub fn set_image(
        &self,
        serial: u64,
        width: i32,
        height: i32,
        rgba: &[u8],
    ) -> Result<(), Error> {
        let mut err = String::new();
        if self.raw.set_image(serial, width, height, rgba, &mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::InvalidArgument, err))
        }
    }

    /// Remove previously registered image pixels for `serial`.
    pub fn remove_image(&self, serial: u64) {
        self.raw.remove_image(serial);
    }

    /// Clear all registered images.
    pub fn clear_images(&self) {
        self.raw.clear_images();
    }

    /// Whether an image is registered under `serial`.
    pub fn has_image(&self, serial: u64) -> bool {
        self.raw.has_image(serial)
    }
}

/// The cursor/pointer presentation manager: KMS planes, sprite themes, and screencopy.
///
/// It keeps the renderer (it installs itself as the renderer's cursor source),
/// the layout and the seat borrowed for as long as it lives, so they are
/// declared before it and drop after it:
///
/// ```no_run
/// # use luminaria::{core::Display, gpu::{Cursor, SceneRenderer, VulkanOptions},
/// #                 protocol::Seat, shell::OutputLayout};
/// let renderer = SceneRenderer::new(&VulkanOptions::default())?;
/// let display = Display::new()?;
/// let layout = OutputLayout::new()?;
/// let seat = Seat::new(&display, "seat0")?;
/// let _cursor = Cursor::new(&renderer, &layout, &seat)?;
/// seat.set_capabilities(true, true, false); // still usable: the borrow is shared
/// # Ok::<(), luminaria::core::Error>(())
/// ```
///
/// Dropping one of them first does not compile, even though the cursor is not
/// touched again — its own drop is what would read the freed `Seat`:
///
/// ```compile_fail,E0505
/// # use luminaria::{core::Display, gpu::{Cursor, SceneRenderer, VulkanOptions},
/// #                 protocol::Seat, shell::OutputLayout};
/// let renderer = SceneRenderer::new(&VulkanOptions::default())?;
/// let display = Display::new()?;
/// let layout = OutputLayout::new()?;
/// let seat = Seat::new(&display, "seat0")?;
/// let _cursor = Cursor::new(&renderer, &layout, &seat)?;
/// drop(seat); // error[E0505]: cannot move out of `seat` because it is borrowed
/// # Ok::<(), luminaria::core::Error>(())
/// ```
pub struct Cursor<'a> {
    raw: cxx::UniquePtr<ffi::CursorShim>,
    _borrows: Holds<'a>,
}

impl<'a> Cursor<'a> {
    pub fn new(
        renderer: &'a SceneRenderer,
        layout: &'a OutputLayout,
        seat: &'a Seat<'_>,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::cursor_new(&renderer.raw, &layout.raw, &seat.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self {
                raw,
                _borrows: Holds::new(),
            })
        }
    }

    /// Load the cursor theme and start following `seat`'s pointer picture,
    /// also wired to `shapes` (`wp_cursor_shape_manager_v1`) to automatically
    /// show requested cursor shapes.
    pub fn new_with_shapes(
        renderer: &'a SceneRenderer,
        layout: &'a OutputLayout,
        seat: &'a Seat<'_>,
        shapes: &'a crate::protocol::CursorShapeManager<'_>,
    ) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::cursor_new_with_shapes(&renderer.raw, &layout.raw, &seat.raw, &shapes.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self {
                raw,
                _borrows: Holds::new(),
            })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::CursorShim> {
        self.raw.pin_mut()
    }

    /// Put the pointer at (`x`, `y`), kept inside the layout. Returns whether
    /// it moved. Until something moves it, it sits in the middle of the first
    /// output to arrive.
    pub fn move_to(&mut self, x: f64, y: f64) -> bool {
        self.pin().move_to(x, y)
    }

    /// Move it by a relative delta (libinput), kept inside the layout.
    pub fn move_by(&mut self, dx: f64, dy: f64) -> bool {
        self.pin().move_by(dx, dy)
    }

    /// [`Cursor::move_to`] kept inside `bounds` instead of the layout: what a
    /// pointer constraint asks for.
    pub fn move_to_within(&mut self, x: f64, y: f64, bounds: Rect) -> bool {
        self.pin().move_to_within(x, y, bounds)
    }

    /// [`Cursor::move_by`] kept inside `bounds`.
    pub fn move_by_within(&mut self, dx: f64, dy: f64, bounds: Rect) -> bool {
        self.pin().move_by_within(dx, dy, bounds)
    }

    pub fn leave(&mut self) {
        self.pin().leave();
    }

    /// Where the pointer is, in layout coordinates, after the clamping
    /// `move_to`/`move_by` did; `None` before the first move or after `leave()`.
    pub fn position(&self) -> Option<(f64, f64)> {
        let (mut x, mut y) = (0.0, 0.0);
        self.raw.position(&mut x, &mut y).then_some((x, y))
    }

    pub fn refresh(&mut self) {
        self.pin().refresh();
    }
}

/// Outcome of presenting a Frame ledger.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Presented {
    Composited = 0,
    Scanout = 1,
    Unchanged = 2,
    Fallback = 3,
}

impl Presented {
    fn from_raw(code: i32) -> Self {
        match code {
            0 => Self::Composited,
            1 => Self::Scanout,
            2 => Self::Unchanged,
            _ => Self::Fallback,
        }
    }
}

/// The per-output immediate-mode GPU frame ledger.
///
/// It caches textures the [`VulkanRenderer`] owns, so it borrows the renderer.
/// The output is another matter: it belongs to its backend, and a monitor can
/// be unplugged under a live `Frame`. When the output goes, the frame goes with
/// it and every method here becomes a no-op (`submit` and `reset` fail), so
/// drop it along with whatever else was built for that output.
pub struct Frame<'r> {
    raw: cxx::UniquePtr<ffi::FrameShim>,
    _renderer: Holds<'r>,
}

impl<'r> Frame<'r> {
    pub fn new(output: &mut Output, renderer: &'r VulkanRenderer) -> Result<Self, Error> {
        let mut err = String::new();
        let mut o_pin = output.raw.pin_mut();
        let raw = ffi::frame_new(o_pin.as_mut(), &renderer.raw, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Self {
                raw,
                _renderer: Holds::new(),
            })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::FrameShim> {
        self.raw.pin_mut()
    }

    pub fn reset(&mut self, format: DrmFourcc) -> Result<(), Error> {
        let mut err = String::new();
        if self.pin().reset(format as u32, &mut err) {
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Gpu, err))
        }
    }

    pub fn begin(&mut self, viewport: Rect) {
        self.pin()
            .begin(viewport.x, viewport.y, viewport.width, viewport.height);
    }

    pub fn place_rect(&mut self, rect: Rect, color: Color) {
        self.pin().place_rect(
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

    pub fn submit(&mut self, background: Color) -> Result<Presented, Error> {
        let mut err = String::new();
        let res = self.pin().submit(
            background.r,
            background.g,
            background.b,
            background.a,
            &mut err,
        );
        if res < 0 {
            Err(Error::new(ErrorKind::Gpu, err))
        } else {
            Ok(Presented::from_raw(res))
        }
    }

    pub fn send_frame_done(&mut self, time_ms: u32) {
        self.pin().send_frame_done(time_ms);
    }
}

/// Direct client scanout bypass evaluator.
///
/// It borrows nothing: the output it imports into is its backend's, and when
/// that output goes (unplugged, or the backend dropped) the C++ side stops
/// calling into it by itself, so `available()` turns false and `id_for` says no.
pub struct DirectScanout {
    raw: cxx::UniquePtr<ffi::DirectScanoutShim>,
}

impl DirectScanout {
    pub fn new(output: &mut Output) -> Result<Self, Error> {
        let mut o_pin = output.raw.pin_mut();
        let raw = ffi::direct_scanout_new(o_pin.as_mut());
        if raw.is_null() {
            Err(Error::new(ErrorKind::Gpu, "failed to initialize direct scanout"))
        } else {
            Ok(Self { raw })
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::DirectScanoutShim> {
        self.raw.pin_mut()
    }

    pub fn available(&self) -> bool {
        self.raw.available()
    }

    pub fn id_for(&mut self, surface: SurfaceId) -> Option<u32> {
        match self.pin().id_for(surface.0) {
            0 => None,
            id => Some(id),
        }
    }

    pub fn committed(&mut self, surface: SurfaceId) {
        self.pin().committed(surface.0);
    }

    pub fn presented(&mut self) {
        self.pin().presented();
    }

    pub fn clear(&mut self) {
        self.pin().clear();
    }

    pub fn cached(&self) -> usize {
        self.raw.cached()
    }
}
