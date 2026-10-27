//! Utility types: geometry, colors, transforms, regions, keymaps, and pixel layouts.

use std::fmt;
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

impl From<(f32, f32, f32, f32)> for Color {
    fn from((r, g, b, a): (f32, f32, f32, f32)) -> Self {
        Self { r, g, b, a }
    }
}

impl From<Color> for (f32, f32, f32, f32) {
    fn from(c: Color) -> Self {
        (c.r, c.g, c.b, c.a)
    }
}

impl From<[f32; 4]> for Color {
    fn from([r, g, b, a]: [f32; 4]) -> Self {
        Self { r, g, b, a }
    }
}

impl From<Color> for [f32; 4] {
    fn from(c: Color) -> Self {
        [c.r, c.g, c.b, c.a]
    }
}

impl From<(f32, f32, f32)> for Color {
    fn from((r, g, b): (f32, f32, f32)) -> Self {
        Self::rgb(r, g, b)
    }
}

impl From<[f32; 3]> for Color {
    fn from([r, g, b]: [f32; 3]) -> Self {
        Self::rgb(r, g, b)
    }
}

/// A 2D point.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Point<T = f64> {
    pub x: T,
    pub y: T,
}

impl<T> Point<T> {
    pub const fn new(x: T, y: T) -> Self {
        Self { x, y }
    }
}

impl<T> From<(T, T)> for Point<T> {
    fn from((x, y): (T, T)) -> Self {
        Self { x, y }
    }
}

impl<T> From<Point<T>> for (T, T) {
    fn from(p: Point<T>) -> Self {
        (p.x, p.y)
    }
}

impl<T> From<[T; 2]> for Point<T> {
    fn from([x, y]: [T; 2]) -> Self {
        Self { x, y }
    }
}

impl<T> From<Point<T>> for [T; 2] {
    fn from(p: Point<T>) -> Self {
        [p.x, p.y]
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

    pub const fn origin(&self) -> Point<i32> {
        Point::new(self.x, self.y)
    }

    pub const fn size(&self) -> Point<i32> {
        Point::new(self.width, self.height)
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

impl From<(i32, i32, i32, i32)> for Rect {
    fn from((x, y, width, height): (i32, i32, i32, i32)) -> Self {
        Self { x, y, width, height }
    }
}

impl From<Rect> for (i32, i32, i32, i32) {
    fn from(r: Rect) -> Self {
        (r.x, r.y, r.width, r.height)
    }
}

impl From<[i32; 4]> for Rect {
    fn from([x, y, width, height]: [i32; 4]) -> Self {
        Self { x, y, width, height }
    }
}

impl From<Rect> for [i32; 4] {
    fn from(r: Rect) -> Self {
        [r.x, r.y, r.width, r.height]
    }
}

impl From<(Point<i32>, Point<i32>)> for Rect {
    fn from((origin, size): (Point<i32>, Point<i32>)) -> Self {
        Self { x: origin.x, y: origin.y, width: size.x, height: size.y }
    }
}

impl From<Rect> for (Point<i32>, Point<i32>) {
    fn from(r: Rect) -> Self {
        (Point::new(r.x, r.y), Point::new(r.width, r.height))
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

/// `0..=7` is a transform, anything else is not.
impl TryFrom<u32> for Transform {
    type Error = u32;

    fn try_from(value: u32) -> Result<Self, u32> {
        if value <= 7 { Ok(Self::from_u32(value)) } else { Err(value) }
    }
}

impl From<Transform> for u32 {
    fn from(transform: Transform) -> u32 {
        transform.as_u32()
    }
}

/// DRM pixel formats, from the `drm-fourcc` crate: what
/// `SceneRenderer::reset_with_format()` and the scanout queries take.
pub use drm_fourcc::DrmFourcc;

/// An xkb keysym: what a key means in the current layout, as
/// `InputEvent::Key` reports it and as a key binding matches it.
///
/// Every `XKB_KEY_<name>` is an associated constant — `Keysym::Return`,
/// `Keysym::a`, `Keysym::XF86Switch_VT_1` — generated from the xkbcommon the
/// library is built against. Names that start with a digit get a leading `_`.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub struct Keysym(u32);

#[allow(non_upper_case_globals)]
mod keysym_table {
    use super::Keysym;
    include!(concat!(env!("OUT_DIR"), "/keysyms.rs"));
}

unsafe extern "C" {
    fn xkb_keysym_to_lower(keysym: u32) -> u32;
    fn xkb_keysym_get_name(keysym: u32, buffer: *mut std::ffi::c_char, size: usize) -> i32;
}

impl Keysym {
    /// No symbol: a key nothing in the layout maps.
    pub const NONE: Keysym = Keysym(0);

    pub const fn new(raw: u32) -> Self {
        Self(raw)
    }

    pub const fn raw(self) -> u32 {
        self.0
    }

    /// The lower-case form, for matching a binding regardless of Shift or
    /// Caps Lock: `Keysym::Q.to_lower() == Keysym::q`.
    pub fn to_lower(self) -> Self {
        // SAFETY: a pure function of its argument.
        Self(unsafe { xkb_keysym_to_lower(self.0) })
    }

    /// The name xkb gives it ("Return", "q"), or `None` for one it has none for.
    pub fn name(self) -> Option<String> {
        let mut buffer = [0 as std::ffi::c_char; 64];
        // SAFETY: the buffer and its size are what xkb is told.
        let len = unsafe { xkb_keysym_get_name(self.0, buffer.as_mut_ptr(), buffer.len()) };
        let len = usize::try_from(len).ok().filter(|&n| n > 0 && n < buffer.len())?;
        let bytes: Vec<u8> = buffer[..len].iter().map(|&c| c as u8).collect();
        String::from_utf8(bytes).ok()
    }
}

impl From<u32> for Keysym {
    fn from(raw: u32) -> Self {
        Self(raw)
    }
}

impl From<Keysym> for u32 {
    fn from(keysym: Keysym) -> u32 {
        keysym.0
    }
}

impl fmt::Debug for Keysym {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.name() {
            Some(name) => write!(f, "Keysym::{name}"),
            None => write!(f, "Keysym({:#x})", self.0),
        }
    }
}

impl fmt::Display for Keysym {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.name() {
            Some(name) => f.write_str(&name),
            None => write!(f, "{:#x}", self.0),
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
            Err(crate::core::Error::new(crate::core::ErrorKind::InvalidArgument, err))
        } else {
            Ok(Self { raw })
        }
    }

    pub fn from_text(text: &str) -> Result<Self, crate::core::Error> {
        let mut err = String::new();
        let raw = ffi::keymap_state_from_text(text, &mut err);
        if raw.is_null() {
            Err(crate::core::Error::new(crate::core::ErrorKind::InvalidArgument, err))
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

/// One RGBA8 pixel, byte for byte `luminaria::Pixel`: the layout of what
/// [`SceneRenderer::read_back`](crate::gpu::SceneRenderer::read_back) returns
/// and [`Output::commit_frame`](crate::backend::Output::commit_frame) takes.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Pixel {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
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

#[cfg(feature = "mint")]
mod mint_impls {
    use super::*;

    impl<T> From<mint::Point2<T>> for Point<T> {
        fn from(p: mint::Point2<T>) -> Self {
            Self { x: p.x, y: p.y }
        }
    }

    impl<T> From<Point<T>> for mint::Point2<T> {
        fn from(p: Point<T>) -> Self {
            Self { x: p.x, y: p.y }
        }
    }

    impl<T> From<mint::Vector2<T>> for Point<T> {
        fn from(v: mint::Vector2<T>) -> Self {
            Self { x: v.x, y: v.y }
        }
    }

    impl<T> From<Point<T>> for mint::Vector2<T> {
        fn from(p: Point<T>) -> Self {
            Self { x: p.x, y: p.y }
        }
    }

    impl From<mint::Vector4<f32>> for Color {
        fn from(v: mint::Vector4<f32>) -> Self {
            Self { r: v.x, g: v.y, b: v.z, a: v.w }
        }
    }

    impl From<Color> for mint::Vector4<f32> {
        fn from(c: Color) -> Self {
            Self { x: c.r, y: c.g, z: c.b, w: c.a }
        }
    }

    impl From<mint::Vector4<i32>> for Rect {
        fn from(v: mint::Vector4<i32>) -> Self {
            Self { x: v.x, y: v.y, width: v.z, height: v.w }
        }
    }

    impl From<Rect> for mint::Vector4<i32> {
        fn from(r: Rect) -> Self {
            Self { x: r.x, y: r.y, z: r.width, w: r.height }
        }
    }

    impl From<(mint::Point2<i32>, mint::Vector2<i32>)> for Rect {
        fn from((p, v): (mint::Point2<i32>, mint::Vector2<i32>)) -> Self {
            Self { x: p.x, y: p.y, width: v.x, height: v.y }
        }
    }

    impl From<Rect> for (mint::Point2<i32>, mint::Vector2<i32>) {
        fn from(r: Rect) -> Self {
            (
                mint::Point2 { x: r.x, y: r.y },
                mint::Vector2 { x: r.width, y: r.height },
            )
        }
    }
}

#[cfg(feature = "glam")]
mod glam_impls {
    use super::*;

    impl From<glam::Vec4> for Color {
        fn from(v: glam::Vec4) -> Self {
            Self { r: v.x, g: v.y, b: v.z, a: v.w }
        }
    }

    impl From<Color> for glam::Vec4 {
        fn from(c: Color) -> Self {
            Self::new(c.r, c.g, c.b, c.a)
        }
    }

    impl From<glam::DVec2> for Point<f64> {
        fn from(v: glam::DVec2) -> Self {
            Self { x: v.x, y: v.y }
        }
    }

    impl From<Point<f64>> for glam::DVec2 {
        fn from(p: Point<f64>) -> Self {
            Self::new(p.x, p.y)
        }
    }

    impl From<glam::Vec2> for Point<f32> {
        fn from(v: glam::Vec2) -> Self {
            Self { x: v.x, y: v.y }
        }
    }

    impl From<Point<f32>> for glam::Vec2 {
        fn from(p: Point<f32>) -> Self {
            Self::new(p.x, p.y)
        }
    }

    impl From<glam::IVec2> for Point<i32> {
        fn from(v: glam::IVec2) -> Self {
            Self { x: v.x, y: v.y }
        }
    }

    impl From<Point<i32>> for glam::IVec2 {
        fn from(p: Point<i32>) -> Self {
            Self::new(p.x, p.y)
        }
    }

    impl From<glam::IVec4> for Rect {
        fn from(v: glam::IVec4) -> Self {
            Self { x: v.x, y: v.y, width: v.z, height: v.w }
        }
    }

    impl From<Rect> for glam::IVec4 {
        fn from(r: Rect) -> Self {
            Self::new(r.x, r.y, r.width, r.height)
        }
    }

    impl From<(glam::IVec2, glam::IVec2)> for Rect {
        fn from((p, s): (glam::IVec2, glam::IVec2)) -> Self {
            Self { x: p.x, y: p.y, width: s.x, height: s.y }
        }
    }

    impl From<Rect> for (glam::IVec2, glam::IVec2) {
        fn from(r: Rect) -> Self {
            (glam::IVec2::new(r.x, r.y), glam::IVec2::new(r.width, r.height))
        }
    }
}
