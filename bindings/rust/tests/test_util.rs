use luminaria::util::{Color, KeymapState, Point, Rect, Region, Transform, layout_fits};

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
    let resized = r1.resized(
        luminaria::util::edge::RIGHT | luminaria::util::edge::BOTTOM,
        10,
        20,
        (50, 50),
    );
    assert_eq!(resized, Rect::new(10, 20, 110, 220));

    // Conversions
    let tuple_rect: (i32, i32, i32, i32) = r1.into();
    assert_eq!(tuple_rect, (10, 20, 100, 200));
    let from_tuple = Rect::from((10, 20, 100, 200));
    assert_eq!(from_tuple, r1);

    let arr_rect: [i32; 4] = r1.into();
    assert_eq!(arr_rect, [10, 20, 100, 200]);
    let from_arr = Rect::from([10, 20, 100, 200]);
    assert_eq!(from_arr, r1);

    assert_eq!(r1.origin(), Point::new(10, 20));
    assert_eq!(r1.size(), Point::new(100, 200));
    let from_points = Rect::from((Point::new(10, 20), Point::new(100, 200)));
    assert_eq!(from_points, r1);
}

#[test]
fn test_point() {
    let p = Point::new(10.5, 20.25);
    assert_eq!(p.x, 10.5);
    assert_eq!(p.y, 20.25);

    let tuple_p: (f64, f64) = p.into();
    assert_eq!(tuple_p, (10.5, 20.25));
    let from_tuple = Point::from((10.5, 20.25));
    assert_eq!(from_tuple, p);

    let arr_p: [f64; 2] = p.into();
    assert_eq!(arr_p, [10.5, 20.25]);
    let from_arr = Point::from([10.5, 20.25]);
    assert_eq!(from_arr, p);
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

    // Conversions
    let tuple_color: (f32, f32, f32, f32) = c.into();
    assert_eq!(tuple_color, (0.5, 0.25, 1.0, 1.0));
    let from_tuple = Color::from((0.5, 0.25, 1.0, 1.0));
    assert_eq!(from_tuple, c);

    let arr_color: [f32; 4] = c.into();
    assert_eq!(arr_color, [0.5, 0.25, 1.0, 1.0]);
    let from_arr = Color::from([0.5, 0.25, 1.0, 1.0]);
    assert_eq!(from_arr, c);

    let rgb_tuple = Color::from((0.5f32, 0.25f32, 1.0f32));
    assert_eq!(rgb_tuple, c);
    let rgb_arr = Color::from([0.5f32, 0.25f32, 1.0f32]);
    assert_eq!(rgb_arr, c);
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

#[cfg(feature = "mint")]
#[test]
fn test_mint_conversions() {
    let p = Point::new(15.0f64, 30.0f64);
    let mint_p: mint::Point2<f64> = p.into();
    assert_eq!(mint_p.x, 15.0);
    assert_eq!(mint_p.y, 30.0);
    let from_mint_p = Point::from(mint_p);
    assert_eq!(from_mint_p, p);

    let mint_v: mint::Vector2<f64> = p.into();
    assert_eq!(mint_v.x, 15.0);
    assert_eq!(mint_v.y, 30.0);
    let from_mint_v = Point::from(mint_v);
    assert_eq!(from_mint_v, p);

    let c = Color::rgba(0.2, 0.4, 0.6, 0.8);
    let mint_c: mint::Vector4<f32> = c.into();
    assert_eq!(mint_c.x, 0.2);
    assert_eq!(mint_c.y, 0.4);
    assert_eq!(mint_c.z, 0.6);
    assert_eq!(mint_c.w, 0.8);
    let from_mint_c = Color::from(mint_c);
    assert_eq!(from_mint_c, c);

    let r = Rect::new(10, 20, 100, 200);
    let mint_r: mint::Vector4<i32> = r.into();
    assert_eq!(mint_r.x, 10);
    assert_eq!(mint_r.y, 20);
    assert_eq!(mint_r.z, 100);
    assert_eq!(mint_r.w, 200);
    let from_mint_r = Rect::from(mint_r);
    assert_eq!(from_mint_r, r);
}

#[cfg(feature = "glam")]
#[test]
fn test_glam_conversions() {
    let p = Point::new(15.0f64, 30.0f64);
    let glam_p: glam::DVec2 = p.into();
    assert_eq!(glam_p.x, 15.0);
    assert_eq!(glam_p.y, 30.0);
    let from_glam_p = Point::from(glam_p);
    assert_eq!(from_glam_p, p);

    let p32 = Point::new(15.0f32, 30.0f32);
    let glam_v2: glam::Vec2 = p32.into();
    assert_eq!(glam_v2.x, 15.0);
    assert_eq!(glam_v2.y, 30.0);
    let from_glam_v2 = Point::from(glam_v2);
    assert_eq!(from_glam_v2, p32);

    let pi = Point::new(15i32, 30i32);
    let glam_iv2: glam::IVec2 = pi.into();
    assert_eq!(glam_iv2.x, 15);
    assert_eq!(glam_iv2.y, 30);
    let from_glam_iv2 = Point::from(glam_iv2);
    assert_eq!(from_glam_iv2, pi);

    let c = Color::rgba(0.2, 0.4, 0.6, 0.8);
    let glam_c: glam::Vec4 = c.into();
    assert_eq!(glam_c.x, 0.2);
    assert_eq!(glam_c.y, 0.4);
    assert_eq!(glam_c.z, 0.6);
    assert_eq!(glam_c.w, 0.8);
    let from_glam_c = Color::from(glam_c);
    assert_eq!(from_glam_c, c);

    let r = Rect::new(10, 20, 100, 200);
    let glam_r: glam::IVec4 = r.into();
    assert_eq!(glam_r.x, 10);
    assert_eq!(glam_r.y, 20);
    assert_eq!(glam_r.z, 100);
    assert_eq!(glam_r.w, 200);
    let from_glam_r = Rect::from(glam_r);
    assert_eq!(from_glam_r, r);
}
