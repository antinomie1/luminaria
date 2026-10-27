//! The text bridge: `import luminaria.text;` on the C++ side.
//!
//! Cargo feature `text`. Glyph rasterization and font metrics, which need fcft
//! and fontconfig — a dependency a compositor that draws no text should not
//! pay. Nothing here is shared with the other bridges.

#[cxx::bridge(namespace = "lum")]
pub(crate) mod ffi {
    unsafe extern "C++" {
        include!("luminaria/luminaria_rs_text.h");

        type FontShim;
        fn font_open(pattern: &str) -> UniquePtr<FontShim>;
        fn height(self: &FontShim) -> i32;
        fn ascent(self: &FontShim) -> i32;
        fn measure(self: &FontShim, utf8: &str) -> i32;
        /// Draws into tightly packed RGBA pixels (`width * height * 4` bytes)
        /// and returns the pen x after the last glyph that fit.
        fn draw(
            self: &FontShim,
            canvas_rgba: &mut [u8],
            width: i32,
            height: i32,
            x: i32,
            baseline: i32,
            utf8: &str,
            r: f32,
            g: f32,
            b: f32,
            a: f32,
            right_edge: i32,
        ) -> i32;

        fn text_decode_utf8(utf8: &str) -> Vec<u32>;
    }
}
