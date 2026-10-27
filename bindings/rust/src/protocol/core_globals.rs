//! The globals every compositor advertises, created together.

use crate::core::{Display, Error};

use super::{
    Compositor, CursorShapeManager, DataDeviceManager, FractionalScaleManager, Presentation,
    PrimarySelectionManager, Seat, SinglePixelBufferManager, Subcompositor, Viewporter,
    XdgDecorationManager, XdgShell,
};

/// wl_shm, wl_compositor, wl_subcompositor, xdg-shell, wl_seat, the clipboard
/// and the primary selection, single-pixel buffers, presentation-time,
/// viewporter, fractional-scale, cursor-shape and xdg-decoration — what a
/// usable compositor needs whatever its policy is, as the C++ `CoreGlobals`.
///
/// It borrows the [`Display`], like every global. The two selection managers
/// borrow the seat, which it owns, so it can be passed around as one value
/// instead of a dozen locals the borrow checker has to order.
///
/// ```no_run
/// # use luminaria::{core::Display, protocol::CoreGlobals};
/// let display = Display::new()?;
/// let globals = CoreGlobals::new(&display)?;
/// globals.seat().set_capabilities(true, true, false);
/// # Ok::<(), luminaria::core::Error>(())
/// ```
pub struct CoreGlobals<'d> {
    // Before `seat`, so they drop before it: both keep the C++ seat.
    data_device: DataDeviceManager<'d>,
    primary_selection: PrimarySelectionManager<'d>,
    compositor: Compositor<'d>,
    subcompositor: Subcompositor<'d>,
    shell: XdgShell<'d>,
    single_pixel_buffer: SinglePixelBufferManager<'d>,
    presentation: Presentation<'d>,
    viewporter: Viewporter<'d>,
    fractional_scale: FractionalScaleManager<'d>,
    cursor_shape: CursorShapeManager<'d>,
    decoration: XdgDecorationManager<'d>,
    // Boxed so it never moves while the managers above point at it.
    seat: Box<Seat<'d>>,
}

impl<'d> CoreGlobals<'d> {
    /// The globals, on a seat named "seat0".
    pub fn new(display: &'d Display) -> Result<Self, Error> {
        Self::with_seat_name(display, "seat0")
    }

    pub fn with_seat_name(display: &'d Display, seat_name: &str) -> Result<Self, Error> {
        display.init_shm()?;
        let seat = Box::new(Seat::new(display, seat_name)?);
        // SAFETY: the seat is on the heap and never moves or is lent out
        // mutably, and the two managers given this reference are fields
        // declared before `seat`, so they are dropped before it. The `'d` it
        // claims is therefore never outlived by a use.
        let seat_ref: &'d Seat<'d> = unsafe { &*std::ptr::from_ref::<Seat<'d>>(&*seat) };
        Ok(Self {
            data_device: DataDeviceManager::new(display, seat_ref)?,
            primary_selection: PrimarySelectionManager::new(display, seat_ref)?,
            compositor: Compositor::new(display)?,
            subcompositor: Subcompositor::new(display)?,
            shell: XdgShell::new(display)?,
            single_pixel_buffer: SinglePixelBufferManager::new(display)?,
            presentation: Presentation::new(display)?,
            viewporter: Viewporter::new(display)?,
            fractional_scale: FractionalScaleManager::new(display)?,
            cursor_shape: CursorShapeManager::new(display)?,
            decoration: XdgDecorationManager::new(display)?,
            seat,
        })
    }

    /// The xdg-shell global, with the library answering window-state
    /// requests; see [`XdgShell::forward_window_requests`] to answer them.
    pub fn forward_window_requests(mut self) -> Self {
        self.shell = self.shell.forward_window_requests();
        self
    }

    pub fn compositor(&self) -> &Compositor<'d> {
        &self.compositor
    }
    pub fn subcompositor(&self) -> &Subcompositor<'d> {
        &self.subcompositor
    }
    pub fn shell(&self) -> &XdgShell<'d> {
        &self.shell
    }
    pub fn seat(&self) -> &Seat<'d> {
        &self.seat
    }
    pub fn data_device(&self) -> &DataDeviceManager<'d> {
        &self.data_device
    }
    pub fn primary_selection(&self) -> &PrimarySelectionManager<'d> {
        &self.primary_selection
    }
    pub fn single_pixel_buffer(&self) -> &SinglePixelBufferManager<'d> {
        &self.single_pixel_buffer
    }
    pub fn presentation(&self) -> &Presentation<'d> {
        &self.presentation
    }
    pub fn viewporter(&self) -> &Viewporter<'d> {
        &self.viewporter
    }
    pub fn fractional_scale(&self) -> &FractionalScaleManager<'d> {
        &self.fractional_scale
    }
    pub fn cursor_shape(&self) -> &CursorShapeManager<'d> {
        &self.cursor_shape
    }
    /// Answers client-side: luminaria draws no title bars.
    pub fn decoration(&self) -> &XdgDecorationManager<'d> {
        &self.decoration
    }
}
