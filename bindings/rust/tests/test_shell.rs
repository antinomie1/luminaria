use luminaria::protocol::SurfaceId;
use luminaria::shell::*;
use luminaria::util::{Color, Rect};

#[test]
fn test_scene_hit_test() {
    let items = vec![
        SceneItem::rect(Rect::new(0, 0, 1000, 1000), Color::rgb(0.0, 0.0, 0.0)).with_tag(1),
        SceneItem::surface(SurfaceId(42), Rect::new(50, 50, 200, 200), 50, 50).with_tag(2),
        SceneItem::border(Rect::new(50, 50, 200, 200), 5, Color::rgb(1.0, 1.0, 1.0)).with_tag(3),
    ];

    // Hit inside the surface
    let hit = scene_hit_test(&items, 100.0, 100.0);
    assert!(hit.hit);

    // Hit on the background
    let hit_bg = scene_hit_test(&items, 500.0, 500.0);
    assert!(hit_bg.hit);
    assert_eq!(hit_bg.tag, 1);

    // Hit outside
    let hit_out = scene_hit_test(&items, 2000.0, 2000.0);
    assert!(!hit_out.hit);
}

#[test]
fn test_pointer_clamping() {
    let mut ptr = Pointer::new();
    assert!(!ptr.is_present());

    let bounds = Rect::new(0, 0, 1920, 1080);
    assert!(ptr.move_to(100.0, 200.0, bounds));
    assert!(ptr.is_present());
    assert_eq!(ptr.x(), 100.0);
    assert_eq!(ptr.y(), 200.0);

    // Clamp beyond boundary
    ptr.move_to(3000.0, 3000.0, bounds);
    assert!(ptr.x() < 1920.0);
    assert!(ptr.y() < 1080.0);

    assert!(ptr.leave());
    assert!(!ptr.is_present());
}

#[test]
fn test_cpu_compositor() {
    let mut comp = CpuCompositor::new().expect("CpuCompositor failed");
    comp.composite_rect(Rect::new(10, 10, 50, 50), Color::rgb(1.0, 0.0, 0.0));
    let pixels = comp.finish(100, 100, Color::rgb(0.0, 0.0, 0.0));
    assert_eq!(pixels.len(), 100 * 100 * 4);
}

#[test]
fn test_popup_tree() {
    let mut tree = PopupTree::new().expect("PopupTree failed");
    assert!(!tree.contains(SurfaceId(123)));
    assert!(!tree.dismiss_grabs_outside(Some(SurfaceId(123))));
}
