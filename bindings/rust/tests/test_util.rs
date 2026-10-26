use luminaria::util::{layout_fits, Color, KeymapState, Rect, Region, Transform};

#[test]
fn test_rect_geometry() {
    let r1 = Rect::new(10, 20, 100, 200);
    assert!(!r1.is_empty());
    assert!(r1.contains(10.0, 20.0));
    assert!(r1.contains(109.0, 219.0));
    assert!(!r1.contains(110.0, 220.0));

    let r2 = Rect::new(50, 50, 100, 100);
    assert!(r1.intersects(&r2));

    let inter = r1.intersection(&r2).unwrap();
    assert_eq!(inter, Rect::new(50, 50, 60, 100));

    let united = r1.united(&r2);
    assert_eq!(united, Rect::new(10, 20, 140, 200));

    // Resizing
    let resized = r1.resized(luminaria::util::edge::RIGHT | luminaria::util::edge::BOTTOM, 10, 20, (50, 50));
    assert_eq!(resized, Rect::new(10, 20, 110, 220));
}

#[test]
fn test_color_conversions() {
    let c = Color::rgb(0.5, 0.25, 1.0);
    assert_eq!(c.a, 1.0);

    let pm = Color::rgba(0.5, 0.25, 1.0, 0.5).premultiplied();
    assert_eq!(pm.r, 0.25);
    assert_eq!(pm.g, 0.125);
    assert_eq!(pm.b, 0.5);
    assert_eq!(pm.a, 0.5);

    let packed = Color::rgba(1.0, 0.0, 0.0, 1.0).to_argb8888();
    assert_eq!(packed, 0xffff0000);
    let unpacked = Color::from_argb8888(packed);
    assert!((unpacked.r - 1.0).abs() < 0.01);
    assert!((unpacked.a - 1.0).abs() < 0.01);
}

#[test]
fn test_transform() {
    let t = Transform::Rotate90;
    assert!(t.swaps_axes());
    assert!(!t.flipped());
    assert_eq!(t.rotation(), 90);
    assert_eq!(t.invert(), Transform::Rotate270);
    assert_eq!(t.apply_size(1920, 1080), (1080, 1920));
    assert_eq!(t.apply_point(1920, 1080, 10, 20), (1060, 10));
}

#[test]
fn test_region() {
    let mut reg = Region::new();
    assert!(reg.is_empty());

    reg.add(Rect::new(0, 0, 100, 100));
    assert!(!reg.is_empty());
    assert!(reg.contains(50, 50));
    assert!(!reg.contains(150, 50));

    reg.subtract_rect(Rect::new(20, 20, 60, 60));
    assert!(!reg.contains(50, 50));
    assert!(reg.contains(10, 10));

    let rects = reg.rects();
    assert!(!rects.is_empty());

    reg.clear();
    assert!(reg.is_empty());
}

#[test]
fn test_keymap() {
    let keymap = KeymapState::from_layout("us").expect("failed to compile us keymap");
    assert!(!keymap.text().is_empty());
    // Evdev code 1 is KEY_ESC
    let sym = keymap.keysym(1);
    assert_ne!(sym, 0);
}

#[test]
fn test_layout_fits() {
    // 100x100 4bpp, stride 400
    assert!(layout_fits(100, 100, 400, 0, 40000));
    // Buffer too small
    assert!(!layout_fits(100, 100, 400, 0, 30000));
    // Stride too small for width
    assert!(!layout_fits(100, 100, 100, 0, 40000));
}
