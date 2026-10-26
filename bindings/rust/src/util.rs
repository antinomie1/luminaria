//! Utility types: geometry, colors, transforms, regions, keymaps, and pixel layouts.

use std::pin::Pin;

use crate::ffi;

/// An RGBA colour, each channel in `0.0..=1.0`, not premultiplied.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Color {
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
}

impl Color {
    pub const fn rgb(r: f32, g: f32, b: f32) -> Self {
        Self { r, g, b, a: 1.0 }
    }

    pub const fn rgba(r: f32, g: f32, b: f32, a: f32) -> Self {
        Self { r, g, b, a }
    }

    pub fn premultiplied(&self) -> Self {
        Self {
            r: self.r * self.a,
            g: self.g * self.a,
            b: self.b * self.a,
            a: self.a,
        }
    }

    pub fn to_argb8888(&self) -> u32 {
        let a = (self.a.clamp(0.0, 1.0) * 255.0).round() as u32;
        let r = (self.r.clamp(0.0, 1.0) * 255.0).round() as u32;
        let g = (self.g.clamp(0.0, 1.0) * 255.0).round() as u32;
        let b = (self.b.clamp(0.0, 1.0) * 255.0).round() as u32;
        (a << 24) | (r << 16) | (g << 8) | b
    }

    pub fn from_argb8888(v: u32) -> Self {
        let a = ((v >> 24) & 0xff) as f32 / 255.0;
        let r = ((v >> 16) & 0xff) as f32 / 255.0;
        let g = ((v >> 8) & 0xff) as f32 / 255.0;
        let b = (v & 0xff) as f32 / 255.0;
        Self { r, g, b, a }
    }
}

/// A rectangle in global logical coordinates.
pub use ffi::Rect;

/// The bits of an xdg_toplevel resize edge, as [`Rect::resized`] takes them.
pub mod edge {
    pub const TOP: u32 = 1;
    pub const BOTTOM: u32 = 2;
    pub const LEFT: u32 = 4;
    pub const RIGHT: u32 = 8;
}

impl Rect {
    pub const fn new(x: i32, y: i32, width: i32, height: i32) -> Self {
        Self {
            x,
            y,
            width,
            height,
        }
    }

    pub const fn is_empty(&self) -> bool {
        self.width <= 0 || self.height <= 0
    }

    pub fn contains(&self, x: f64, y: f64) -> bool {
        x >= self.x as f64
            && y >= self.y as f64
            && x < (self.x + self.width) as f64
            && y < (self.y + self.height) as f64
    }

    pub fn intersects(&self, other: &Rect) -> bool {
        if self.is_empty() || other.is_empty() {
            return false;
        }
        self.x < other.x + other.width
            && self.x + self.width > other.x
            && self.y < other.y + other.height
            && self.y + self.height > other.y
    }

    pub fn intersection(&self, other: &Rect) -> Option<Rect> {
        let x0 = self.x.max(other.x);
        let y0 = self.y.max(other.y);
        let x1 = (self.x + self.width).min(other.x + other.width);
        let y1 = (self.y + self.height).min(other.y + other.height);
        if x1 <= x0 || y1 <= y0 {
            None
        } else {
            Some(Rect::new(x0, y0, x1 - x0, y1 - y0))
        }
    }

    pub fn united(&self, other: &Rect) -> Rect {
        if self.is_empty() {
            return *other;
        }
        if other.is_empty() {
            return *self;
        }
        let x0 = self.x.min(other.x);
        let y0 = self.y.min(other.y);
        let x1 = (self.x + self.width).max(other.x + other.width);
        let y1 = (self.y + self.height).max(other.y + other.height);
        Rect::new(x0, y0, x1 - x0, y1 - y0)
    }

    /// The rectangle an interactive resize from `self` reaches after the
    /// pointer moved by (`dx`, `dy`).
    pub fn resized(&self, edges: u32, dx: i32, dy: i32, min: (i32, i32)) -> Rect {
        let (min_w, min_h) = (min.0.max(1), min.1.max(1));
        let mut r = *self;
        if edges & edge::LEFT != 0 {
            let w = (self.width - dx).max(min_w);
            r.x = self.x + self.width - w;
            r.width = w;
        } else if edges & edge::RIGHT != 0 {
            r.width = (self.width + dx).max(min_w);
        }
        if edges & edge::TOP != 0 {
            let h = (self.height - dy).max(min_h);
            r.y = self.y + self.height - h;
            r.height = h;
        } else if edges & edge::BOTTOM != 0 {
            r.height = (self.height + dy).max(min_h);
        }
        r
    }
}

/// How an output is rotated / flipped (wl_output.transform).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Transform {
    #[default]
    Normal = 0,
    Rotate90 = 1,
    Rotate180 = 2,
    Rotate270 = 3,
    Flipped = 4,
    Flipped90 = 5,
    Flipped180 = 6,
    Flipped270 = 7,
}

impl Transform {
    pub const fn from_u32(val: u32) -> Self {
        match val & 7 {
            0 => Self::Normal,
            1 => Self::Rotate90,
            2 => Self::Rotate180,
            3 => Self::Rotate270,
            4 => Self::Flipped,
            5 => Self::Flipped90,
            6 => Self::Flipped180,
            7 => Self::Flipped270,
            _ => unreachable!(),
        }
    }

    pub const fn as_u32(self) -> u32 {
        self as u32
    }

    pub const fn swaps_axes(self) -> bool {
        (self as u32 & 1) != 0
    }

    pub const fn flipped(self) -> bool {
        (self as u32) >= 4
    }

    pub const fn rotation(self) -> i32 {
        ((self as u32 & 3) * 90) as i32
    }

    pub const fn invert(self) -> Self {
        match self {
            Self::Normal => Self::Normal,
            Self::Rotate90 => Self::Rotate270,
            Self::Rotate180 => Self::Rotate180,
            Self::Rotate270 => Self::Rotate90,
            Self::Flipped => Self::Flipped,
            Self::Flipped90 => Self::Flipped90,
            Self::Flipped180 => Self::Flipped180,
            Self::Flipped270 => Self::Flipped270,
        }
    }

    pub fn apply_size(self, width: i32, height: i32) -> (i32, i32) {
        if self.swaps_axes() {
            (height, width)
        } else {
            (width, height)
        }
    }

    pub fn apply_point(self, width: i32, height: i32, x: i32, y: i32) -> (i32, i32) {
        match self {
            Self::Normal => (x, y),
            Self::Rotate90 => (height - y, x),
            Self::Rotate180 => (width - x, height - y),
            Self::Rotate270 => (y, width - x),
            Self::Flipped => (width - x, y),
            Self::Flipped90 => (height - y, width - x),
            Self::Flipped180 => (x, height - y),
            Self::Flipped270 => (y, x),
        }
    }
}

/// A set of disjoint non-overlapping rectangles.
pub struct Region {
    raw: cxx::UniquePtr<ffi::RegionShim>,
}

impl Region {
    pub fn new() -> Self {
        Self {
            raw: ffi::region_new(),
        }
    }

    pub fn from_rect(rect: Rect) -> Self {
        Self {
            raw: ffi::region_from_box(rect.x, rect.y, rect.width, rect.height),
        }
    }

    fn pin(&mut self) -> Pin<&mut ffi::RegionShim> {
        self.raw.pin_mut()
    }

    pub fn add(&mut self, rect: Rect) {
        self.pin().add(rect.x, rect.y, rect.width, rect.height);
    }

    pub fn subtract_rect(&mut self, rect: Rect) {
        self.pin().subtract_box(rect.x, rect.y, rect.width, rect.height);
    }

    pub fn subtract(&mut self, other: &Region) {
        self.pin().subtract_region(&other.raw);
    }

    pub fn intersect_rect(&mut self, rect: Rect) {
        self.pin().intersect_box(rect.x, rect.y, rect.width, rect.height);
    }

    pub fn intersect(&mut self, other: &Region) {
        self.pin().intersect_region(&other.raw);
    }

    pub fn is_empty(&self) -> bool {
        self.raw.empty()
    }

    pub fn clear(&mut self) {
        self.pin().clear();
    }

    pub fn contains(&self, x: i32, y: i32) -> bool {
        self.raw.contains(x, y)
    }

    pub fn extents(&self) -> Rect {
        self.raw.extents()
    }

    pub fn rects(&self) -> Vec<Rect> {
        self.raw.rects()
    }
}

impl Default for Region {
    fn default() -> Self {
        Self::new()
    }
}

/// An xkb keymap wrapper.
pub struct KeymapState {
    raw: cxx::UniquePtr<ffi::KeymapStateShim>,
}

impl KeymapState {
    pub fn from_layout(layout: &str) -> Result<Self, crate::core::Error> {
        let mut err = String::new();
        let raw = ffi::keymap_state_from_layout(layout, &mut err);
        if raw.is_null() {
            Err(crate::core::Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn from_text(text: &str) -> Result<Self, crate::core::Error> {
        let mut err = String::new();
        let raw = ffi::keymap_state_from_text(text, &mut err);
        if raw.is_null() {
            Err(crate::core::Error(err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn text(&self) -> String {
        self.raw.text()
    }

    pub fn keysym(&self, code: u32) -> u32 {
        self.raw.key_keysym(code)
    }

    pub fn update_key(&mut self, code: u32, pressed: bool) {
        self.raw.pin_mut().update_key(code, pressed);
    }

    pub fn modifiers(&self) -> (u32, u32, u32, u32) {
        (
            self.raw.modifiers_depressed(),
            self.raw.modifiers_latched(),
            self.raw.modifiers_locked(),
            self.raw.modifiers_group(),
        )
    }
}

/// Single pixel (BGRA).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Pixel {
    pub b: u8,
    pub g: u8,
    pub r: u8,
    pub a: u8,
}

/// Single plane of a Linux dmabuf.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DmabufPlane {
    pub fd: i32,
    pub width: i32,
    pub height: i32,
    pub format: u32,
    pub offset: u32,
    pub stride: u32,
    pub modifier: u64,
}

/// Validates whether a pixel buffer layout safely fits within a slice/memory region.
pub fn layout_fits(
    width: i32,
    height: i32,
    stride: u32,
    offset: u32,
    total_size: usize,
) -> bool {
    ffi::layout_fits_raw(width, height, stride, offset, total_size)
}

/// One display mode a monitor supports.
pub use ffi::Mode;
