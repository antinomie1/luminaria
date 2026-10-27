//! Colour: `wp_color_manager_v1` and `wp_color_representation_manager_v1`.
//!
//! Clients say what their pixels mean — a colour space for the surface, a
//! preferred one to render in, how alpha and YUV are to be read — and the
//! compositor says what its displays are. Descriptions are parametric:
//! primaries, a transfer function, luminances, a mastering volume.
//!
//! **This is the negotiation, not the conversion.** Neither renderer converts
//! between colour spaces; both composite in the encoding the client used, which
//! is right for sRGB on an sRGB display and wrong for anything else.
//! [`ColorSupport`] is how the compositor says what it can honour:
//! [`ColorSupport::SrgbOnly`] (the default) advertises sRGB and nothing else, so
//! clients render sRGB for it and anything else is a protocol error rather than
//! a quietly wrong picture; [`ColorSupport::Full`] is for a compositor whose own
//! pipeline reads [`SurfaceId::image_description`] and converts.

use super::SurfaceId;
use crate::core::{Display, Error, ErrorKind, Holds};
use crate::ffi;

/// Named colour primaries (the protocol's numbering).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum Primaries {
    Srgb = 1,
    PalM = 2,
    Pal = 3,
    Ntsc = 4,
    GenericFilm = 5,
    Bt2020 = 6,
    Cie1931Xyz = 7,
    DciP3 = 8,
    DisplayP3 = 9,
    AdobeRgb = 10,
}

impl Primaries {
    fn from_u32(value: u32) -> Option<Self> {
        Some(match value {
            1 => Self::Srgb,
            2 => Self::PalM,
            3 => Self::Pal,
            4 => Self::Ntsc,
            5 => Self::GenericFilm,
            6 => Self::Bt2020,
            7 => Self::Cie1931Xyz,
            8 => Self::DciP3,
            9 => Self::DisplayP3,
            10 => Self::AdobeRgb,
            _ => return None,
        })
    }
}

/// Named transfer functions (the protocol's numbering).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum TransferFunction {
    Bt1886 = 1,
    Gamma22 = 2,
    Gamma28 = 3,
    St240 = 4,
    ExtLinear = 5,
    Log100 = 6,
    Log316 = 7,
    Xvycc = 8,
    /// Deprecated in v2 in favour of [`CompoundPower24`](Self::CompoundPower24).
    Srgb = 9,
    ExtSrgb = 10,
    /// HDR10's curve.
    St2084Pq = 11,
    St428 = 12,
    /// Broadcast HDR.
    Hlg = 13,
    /// sRGB's actual curve.
    CompoundPower24 = 14,
}

impl TransferFunction {
    fn from_u32(value: u32) -> Option<Self> {
        Some(match value {
            1 => Self::Bt1886,
            2 => Self::Gamma22,
            3 => Self::Gamma28,
            4 => Self::St240,
            5 => Self::ExtLinear,
            6 => Self::Log100,
            7 => Self::Log316,
            8 => Self::Xvycc,
            9 => Self::Srgb,
            10 => Self::ExtSrgb,
            11 => Self::St2084Pq,
            12 => Self::St428,
            13 => Self::Hlg,
            14 => Self::CompoundPower24,
            _ => return None,
        })
    }
}

/// CIE 1931 xy of red, green, blue and white.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PrimarySet {
    pub red: (f64, f64),
    pub green: (f64, f64),
    pub blue: (f64, f64),
    pub white: (f64, f64),
}

impl PrimarySet {
    fn from_slice(v: &[f64]) -> Option<Self> {
        (v.len() >= 8).then(|| Self {
            red: (v[0], v[1]),
            green: (v[2], v[3]),
            blue: (v[4], v[5]),
            white: (v[6], v[7]),
        })
    }
    fn to_vec(self) -> Vec<f64> {
        vec![
            self.red.0,
            self.red.1,
            self.green.0,
            self.green.1,
            self.blue.0,
            self.blue.1,
            self.white.0,
            self.white.1,
        ]
    }
}

/// A colour space, as a client or a display declares it.
#[derive(Debug, Clone, PartialEq)]
pub struct ImageDescription {
    pub primaries: PrimarySet,
    /// `None` for custom chromaticities.
    pub named_primaries: Option<Primaries>,
    /// A named curve, or — when `None` — the pure power `power`.
    pub transfer_function: Option<TransferFunction>,
    pub power: f64,
    /// Luminance of the primary colour volume and the reference white, in cd/m².
    pub min_luminance: f64,
    pub max_luminance: f64,
    pub reference_luminance: f64,
    /// The mastering display the content was graded on.
    pub target_primaries: Option<PrimarySet>,
    pub target_luminance: Option<(f64, f64)>,
    /// Brightest pixel / brightest frame average, cd/m².
    pub max_cll: Option<u32>,
    pub max_fall: Option<u32>,
    /// The same colour space has the same identity across clients; 0 until the
    /// colour manager assigns one.
    pub identity: u64,
}

impl ImageDescription {
    /// Plain sRGB: the colour space nothing needs converting from.
    pub fn srgb() -> Self {
        Self {
            primaries: PrimarySet {
                red: (0.640, 0.330),
                green: (0.300, 0.600),
                blue: (0.150, 0.060),
                white: (0.3127, 0.3290),
            },
            named_primaries: Some(Primaries::Srgb),
            transfer_function: Some(TransferFunction::CompoundPower24),
            power: 0.0,
            min_luminance: 0.2,
            max_luminance: 80.0,
            reference_luminance: 80.0,
            target_primaries: None,
            target_luminance: None,
            max_cll: None,
            max_fall: None,
            identity: 0,
        }
    }

    /// HDR10 as delivered: BT.2020 primaries, PQ, 203-nit diffuse white.
    pub fn bt2100_pq() -> Self {
        Self {
            primaries: PrimarySet {
                red: (0.708, 0.292),
                green: (0.170, 0.797),
                blue: (0.131, 0.046),
                white: (0.3127, 0.3290),
            },
            named_primaries: Some(Primaries::Bt2020),
            transfer_function: Some(TransferFunction::St2084Pq),
            min_luminance: 0.005,
            max_luminance: 10000.0,
            reference_luminance: 203.0,
            ..Self::srgb()
        }
    }

    /// An HDR transfer function: pixels are not in the 0..1 SDR range.
    pub fn is_hdr(&self) -> bool {
        matches!(
            self.transfer_function,
            Some(TransferFunction::St2084Pq | TransferFunction::Hlg | TransferFunction::ExtLinear)
        )
    }

    /// sRGB primaries with an SDR curve that is, or is within a hair of,
    /// sRGB's: what the renderers already composite in, so no conversion is owed.
    pub fn is_srgb(&self) -> bool {
        if self.named_primaries != Some(Primaries::Srgb) {
            return false;
        }
        match self.transfer_function {
            None => (2.1..=2.3).contains(&self.power),
            Some(tf) => matches!(
                tf,
                TransferFunction::CompoundPower24
                    | TransferFunction::Srgb
                    | TransferFunction::Gamma22
                    | TransferFunction::Bt1886
            ),
        }
    }

    pub(crate) fn from_raw(raw: &ffi::ImageDescriptionRaw) -> Self {
        let srgb = Self::srgb();
        Self {
            primaries: PrimarySet::from_slice(&raw.primaries).unwrap_or(srgb.primaries),
            named_primaries: Primaries::from_u32(raw.named_primaries),
            transfer_function: TransferFunction::from_u32(raw.transfer_function),
            power: raw.power,
            min_luminance: raw.min_luminance,
            max_luminance: raw.max_luminance,
            reference_luminance: raw.reference_luminance,
            target_primaries: PrimarySet::from_slice(&raw.target_primaries),
            target_luminance: raw
                .has_target_luminance
                .then_some((raw.target_min_luminance, raw.target_max_luminance)),
            max_cll: u32::try_from(raw.max_cll).ok(),
            max_fall: u32::try_from(raw.max_fall).ok(),
            identity: raw.identity,
        }
    }

    pub(crate) fn to_raw(&self) -> ffi::ImageDescriptionRaw {
        ffi::ImageDescriptionRaw {
            named_primaries: self.named_primaries.map_or(0, |p| p as u32),
            primaries: self.primaries.to_vec(),
            transfer_function: self.transfer_function.map_or(0, |t| t as u32),
            power: self.power,
            min_luminance: self.min_luminance,
            max_luminance: self.max_luminance,
            reference_luminance: self.reference_luminance,
            target_primaries: self.target_primaries.map(PrimarySet::to_vec).unwrap_or_default(),
            has_target_luminance: self.target_luminance.is_some(),
            target_min_luminance: self.target_luminance.map_or(0.0, |t| t.0),
            target_max_luminance: self.target_luminance.map_or(0.0, |t| t.1),
            max_cll: self.max_cll.map_or(-1, i64::from),
            max_fall: self.max_fall.map_or(-1, i64::from),
            identity: self.identity,
        }
    }
}

/// What the compositor is willing to be told about.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ColorSupport {
    /// sRGB and nothing else: the right thing for a compositor with no colour
    /// pipeline.
    #[default]
    SrgbOnly,
    /// Every named primary and transfer function and every parametric feature:
    /// for a compositor whose renderer converts between them.
    Full,
}

/// The `wp_color_manager_v1` global (version 3).
pub struct ColorManager<'d> {
    pub(crate) raw: cxx::UniquePtr<ffi::ColorManagerShim>,
    _display: Holds<'d>,
}

impl<'d> ColorManager<'d> {
    pub fn new(display: &'d Display, support: ColorSupport) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::color_manager_new(&display.raw, support == ColorSupport::Full, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }

    /// What `output` (its `wl_output`) is: the colour space a client should
    /// render for to look right on it. Clients that already asked are told it
    /// changed. Outputs default to sRGB.
    pub fn set_output_description(
        &self,
        output: &crate::protocol::OutputGlobal<'_>,
        description: &ImageDescription,
    ) {
        self.raw
            .set_output_description(&output.raw, &description.to_raw());
    }

    /// The description of `output` as set, or sRGB.
    pub fn output_description(&self, output: &crate::protocol::OutputGlobal<'_>) -> ImageDescription {
        let mut raw = ImageDescription::srgb().to_raw();
        self.raw.output_description(&output.raw, &mut raw);
        ImageDescription::from_raw(&raw)
    }

    /// What a surface with no preferred description of its own should render in.
    pub fn set_default_preferred(&self, description: &ImageDescription) {
        self.raw.set_default_preferred(&description.to_raw());
    }

    /// The colour space `surface` would be best rendered in — typically its
    /// output's. Clients watching its feedback object are told.
    pub fn set_preferred(&self, surface: SurfaceId, description: &ImageDescription) {
        self.raw.set_preferred(surface.0, &description.to_raw());
    }
}

/// The `wp_color_representation_manager_v1` global (version 1).
///
/// With `extended` false only plain RGB (premultiplied alpha, identity matrix,
/// full range) is accepted: a compositor whose renderer cannot read YUV should
/// leave it so, and video clients convert to RGB themselves.
pub struct ColorRepresentationManager<'d> {
    #[allow(dead_code)]
    pub(crate) raw: cxx::UniquePtr<ffi::ColorRepresentationManagerShim>,
    _display: Holds<'d>,
}

impl<'d> ColorRepresentationManager<'d> {
    pub fn new(display: &'d Display, extended: bool) -> Result<Self, Error> {
        let mut err = String::new();
        let raw = ffi::color_representation_manager_new(&display.raw, extended, &mut err);
        if raw.is_null() {
            Err(Error::new(ErrorKind::Protocol, err))
        } else {
            Ok(Self {
                raw,
                _display: Holds::new(),
            })
        }
    }
}

/// What a client has said about a surface's pixels, besides their colour space.
/// `None` is the client saying nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ColorRepresentation {
    pub alpha_mode: Option<AlphaMode>,
    pub coefficients: Option<Coefficients>,
    pub range: Option<ColorRange>,
    pub chroma_location: Option<u32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AlphaMode {
    /// The default, and what the renderers composite.
    PremultipliedElectrical,
    PremultipliedOptical,
    Straight,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Coefficients {
    Identity,
    Bt709,
    Fcc,
    Bt601,
    Smpte240,
    Bt2020,
    Bt2020Cl,
    Ictcp,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorRange {
    Full,
    Limited,
}

/// How a surface's committed state describes its colour, read off the surface
/// after the commit that carried it.
impl SurfaceId {
    /// The colour space the client declared for this surface, or `None` when it
    /// declared none (read that as sRGB).
    pub fn image_description(&self) -> Option<ImageDescription> {
        let mut raw = ImageDescription::srgb().to_raw();
        ffi::surface_image_description(self.0, &mut raw).then(|| ImageDescription::from_raw(&raw))
    }

    /// The rendering intent that goes with [`image_description`](Self::image_description)
    /// (the protocol's numbering: 0 perceptual, 1 relative, 2 saturation,
    /// 3 absolute, 4 relative with black point compensation).
    pub fn render_intent(&self) -> u32 {
        ffi::surface_render_intent(self.0)
    }

    /// Alpha mode, YUV matrix and range, chroma siting, as far as the client said.
    pub fn color_representation(&self) -> ColorRepresentation {
        let mut raw = ffi::ColorRepresentationRaw::default();
        if !ffi::surface_color_representation(self.0, &mut raw) {
            return ColorRepresentation::default();
        }
        ColorRepresentation {
            alpha_mode: match raw.alpha_mode {
                1 => Some(AlphaMode::PremultipliedElectrical),
                2 => Some(AlphaMode::PremultipliedOptical),
                3 => Some(AlphaMode::Straight),
                _ => None,
            },
            coefficients: match raw.coefficients {
                1 => Some(Coefficients::Identity),
                2 => Some(Coefficients::Bt709),
                3 => Some(Coefficients::Fcc),
                4 => Some(Coefficients::Bt601),
                5 => Some(Coefficients::Smpte240),
                6 => Some(Coefficients::Bt2020),
                7 => Some(Coefficients::Bt2020Cl),
                8 => Some(Coefficients::Ictcp),
                _ => None,
            },
            range: match raw.range {
                1 => Some(ColorRange::Full),
                2 => Some(ColorRange::Limited),
                _ => None,
            },
            chroma_location: (raw.chroma_location != 0).then_some(raw.chroma_location),
        }
    }
}
