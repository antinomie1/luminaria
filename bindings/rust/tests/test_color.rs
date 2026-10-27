//! wp-color-management and wp-color-representation: the managers come up in
//! both support levels, an output's description round-trips through the
//! compositor, and a surface nobody tagged reads as having no description.
use luminaria::core::Display;
use luminaria::protocol::{
    ColorManager, ColorRepresentationManager, ColorSupport, ImageDescription, OutputGlobal,
    Primaries, SurfaceId, TransferFunction,
};

fn display() -> Display {
    let temp_dir = std::env::temp_dir().join("luminaria_test_color_runtime");
    let _ = std::fs::create_dir_all(&temp_dir);
    unsafe {
        std::env::set_var("XDG_RUNTIME_DIR", &temp_dir);
    }
    Display::new().expect("failed to create display")
}

#[test]
fn descriptions_classify() {
    let srgb = ImageDescription::srgb();
    assert!(srgb.is_srgb() && !srgb.is_hdr());
    let hdr = ImageDescription::bt2100_pq();
    assert!(hdr.is_hdr() && !hdr.is_srgb());
    assert_eq!(hdr.named_primaries, Some(Primaries::Bt2020));
    assert_eq!(hdr.transfer_function, Some(TransferFunction::St2084Pq));
    // A pure 2.2 power curve on sRGB primaries is sRGB for our purposes.
    let gamma = ImageDescription {
        transfer_function: None,
        power: 2.2,
        ..ImageDescription::srgb()
    };
    assert!(gamma.is_srgb());
}

#[test]
fn managers_come_up_and_an_output_description_round_trips() {
    let display = display();
    let sdr = ColorManager::new(&display, ColorSupport::SrgbOnly).expect("ColorManager failed");
    drop(sdr);
    let colors = ColorManager::new(&display, ColorSupport::Full).expect("ColorManager failed");
    let _rep = ColorRepresentationManager::new(&display, false).expect("representation failed");

    let output = OutputGlobal::new(&display, 1920, 1080, "color-test").expect("output failed");
    // Outputs are sRGB until told otherwise.
    assert!(colors.output_description(&output).is_srgb());

    let mut hdr = ImageDescription::bt2100_pq();
    hdr.max_cll = Some(1000);
    hdr.max_fall = Some(400);
    hdr.target_luminance = Some((0.005, 1000.0));
    colors.set_output_description(&output, &hdr);
    let read = colors.output_description(&output);
    assert!(read.is_hdr());
    assert_eq!(read.transfer_function, Some(TransferFunction::St2084Pq));
    assert_eq!(read.named_primaries, Some(Primaries::Bt2020));
    assert_eq!(read.max_cll, Some(1000));
    assert_eq!(read.max_fall, Some(400));
    assert_eq!(read.target_luminance, Some((0.005, 1000.0)));
    assert_ne!(read.identity, 0, "the colour manager assigns an identity");

    // Saying the same thing again is not a change: same identity, and clients
    // that asked are not told anything happened.
    colors.set_output_description(&output, &ImageDescription::srgb());
    let srgb_identity = colors.output_description(&output).identity;
    colors.set_output_description(&output, &ImageDescription::srgb());
    assert_eq!(colors.output_description(&output).identity, srgb_identity);
    assert_ne!(srgb_identity, read.identity, "different spaces, different identities");

    // A surface that is gone has no description and no representation.
    assert_eq!(SurfaceId::INVALID.image_description(), None);
    assert_eq!(SurfaceId::INVALID.color_representation(), Default::default());
    colors.set_preferred(SurfaceId::INVALID, &hdr);
    colors.set_default_preferred(&ImageDescription::srgb());
}
