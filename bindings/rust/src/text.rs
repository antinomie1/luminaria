//! Text: glyph rasterization and font metrics, backed by fcft (`luminaria.text`).
//!
//! Cargo feature `text`. Nothing in the core library needs it — a compositor
//! that draws no text should not link fontconfig — which is why the C++ side is
//! a fourth module rather than part of `luminaria`.
//!
//! ```no_run
//! # use luminaria::text::Font;
//! # use luminaria::util::Color;
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! let mut font = Font::open("monospace:size=12").ok_or("no monospace font")?;
//! let mut canvas = vec![0u8; 200 * 32 * 4];
//! let pen = font.draw(&mut canvas, 200, 32, 4, font.ascent(), "hello",
//!                     Color::rgb(1.0, 1.0, 1.0), 200)?;
//! assert!(pen > 4);
//! # Ok(())
//! # }
//! ```

use crate::core::{Error, ErrorKind};
use crate::ffi_text as ffi;
use crate::util::Color;

/// A rasterizable font, opened from a fontconfig pattern like `"monospace:size=11"`.
pub struct Font {
    raw: cxx::UniquePtr<ffi::FontShim>,
}

impl Font {
    /// `None` when no font matches the pattern or fcft cannot open it.
    pub fn open(pattern: &str) -> Option<Self> {
        let raw = ffi::font_open(pattern);
        (!raw.is_null()).then_some(Self { raw })
    }

    /// Line height in pixels.
    pub fn height(&self) -> i32 {
        self.raw.height()
    }

    /// Distance from the top of the line to the baseline, in pixels.
    pub fn ascent(&self) -> i32 {
        self.raw.ascent()
    }

    /// Advance width of `utf8`, in pixels.
    pub fn measure(&mut self, utf8: &str) -> i32 {
        self.raw.pin_mut().measure(utf8)
    }

    /// Draws `utf8` with its baseline at `baseline`, clipped to `right_edge`.
    ///
    /// `canvas` is tightly packed RGBA8 (`width * height * 4` bytes), the same
    /// layout [`SceneRenderer::read_back`](crate::gpu::SceneRenderer::read_back)
    /// returns and [`Output::commit_frame`](crate::backend::Output::commit_frame)
    /// takes. Returns the pen x after the last glyph that fit; an empty canvas
    /// or a canvas too small for `width * height` draws nothing and returns `x`.
    pub fn draw(
        &mut self,
        canvas: &mut [u8],
        width: i32,
        height: i32,
        x: i32,
        baseline: i32,
        utf8: &str,
        color: Color,
        right_edge: i32,
    ) -> Result<i32, Error> {
        if width <= 0 || height <= 0 {
            return Err(Error::new(
                ErrorKind::InvalidArgument,
                "canvas must be non-empty",
            ));
        }
        let needed = width as usize * height as usize * 4;
        if canvas.len() < needed {
            return Err(Error::new(
                ErrorKind::InvalidArgument,
                format!(
                    "canvas holds {} bytes, {}x{} needs {}",
                    canvas.len(),
                    width,
                    height,
                    needed
                ),
            ));
        }
        Ok(self.raw.pin_mut().draw(
            canvas, width, height, x, baseline, utf8, color.r, color.g, color.b, color.a,
            right_edge,
        ))
    }
}

/// Decodes UTF-8 into code points, replacing malformed sequences with U+FFFD.
pub fn decode_utf8(utf8: &str) -> Vec<char> {
    ffi::text_decode_utf8(utf8)
        .into_iter()
        .filter_map(char::from_u32)
        .collect()
}
