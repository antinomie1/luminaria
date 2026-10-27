//! `luminaria::text`: UTF-8 decoding, font metrics and glyph rasterization.
//! Skips (exit 0 with a note) when the machine has no usable font, which is the
//! common case in a container without fontconfig fonts.

use luminaria::text::{Font, decode_utf8};
use luminaria::util::Color;

#[test]
fn utf8_decoding() {
    assert_eq!(decode_utf8("A"), ['A']);
    assert_eq!(decode_utf8("é中😀"), ['é', '中', '😀']);
    // The C++ decoder replaces malformed bytes with U+FFFD; `&str` cannot hold
    // one, so that branch is only reachable from luminaria's own C++ callers
    // (tests/test_text.cpp covers it).
    assert_eq!(decode_utf8(""), []);
}

#[test]
fn font_metrics_and_drawing() {
    let Some(mut font) = Font::open("monospace:size=12") else {
        eprintln!("skip: no monospace font available for fcft");
        return;
    };
    assert!(font.height() > 0);
    assert!(font.ascent() > 0);
    assert_eq!(font.measure(""), 0);
    assert!(font.measure("Hello") > 0);

    let (width, height) = (128, 32);
    let mut canvas = vec![0u8; (width * height * 4) as usize];
    let pen = font
        .draw(
            &mut canvas,
            width,
            height,
            4,
            font.ascent() + 2,
            "Hi",
            Color::rgb(1.0, 1.0, 1.0),
            width,
        )
        .expect("draw");
    assert!(pen > 4, "pen did not advance");

    // Something was actually rasterized into the alpha channel.
    assert!(
        canvas.chunks_exact(4).any(|px| px[3] != 0),
        "no glyph pixels"
    );

    // A canvas that is too small is refused rather than overrun.
    let mut tiny = vec![0u8; 4];
    assert!(
        font.draw(
            &mut tiny,
            128,
            32,
            0,
            0,
            "Hi",
            Color::rgb(1.0, 1.0, 1.0),
            128
        )
        .is_err()
    );
}
