// luminaria/color_management.cppm — wp_color_manager_v1 and
// wp_color_representation_manager_v1: clients saying what their pixels MEAN.
//
// A video player decoding HDR10 hands the compositor PQ-encoded BT.2020 and
// wants it shown as HDR on a display that can; a photo editor wants Display P3
// shown as Display P3; an ordinary app is sRGB and wants nothing done. Without
// a protocol the compositor guesses sRGB for all of them, which is right for the
// ordinary app and wrong, visibly, for the other two. These two globals are the
// protocol:
//
//   * `ColorManager` (wp-color-management-v1, version 3) — the colour space of a
//     surface (`Surface::image_description()`), of an output (what a client
//     should render for it), and the colour space a client would prefer to
//     render in for a given surface. Descriptions are parametric — primaries,
//     transfer function, luminances, mastering volume — ICC profiles are not
//     supported.
//   * `ColorRepresentationManager` (wp-color-representation-v1) — alpha mode,
//     YUV matrix and range, chroma siting (`Surface::color_representation()`).
//
// THIS LAYER IS THE NEGOTIATION, NOT THE CONVERSION. Neither renderer in the
// library converts between colour spaces; both composite in whatever encoding
// the client used. That is exactly right for sRGB on an sRGB display and wrong
// for anything else. So `ColorSupport` is how the compositor says what it can
// actually honour: the default (`ColorSupport::srgb_only()`) advertises sRGB
// and nothing else, which makes every well-behaved client render sRGB for us,
// and a compositor that implements a colour pipeline widens it with
// `ColorSupport::full()` (or a subset) and reads `Surface::image_description()`
// to drive it. `ImageDescription::is_srgb()` is the "no conversion owed" test.
//
// Output descriptions are the compositor's to give: `set_output_description()`
// says what a display is (sRGB, or BT.2020 PQ at 1000 nits...), and a client
// that asks for it — or for the description it prefers for a surface — is told.

module;

#include "detail/wayland_fwd.h"
#include <cstdint>

#include <wayland-server-core.h>
#include "color-management-v1-protocol.h"
#include "color-representation-v1-protocol.h"

export module luminaria:color_management;

import std;

import :color_description;
import :compositor;
import :display;
import :expected;
import :output_global;
import :protocol_helper;
import :signal;

export namespace luminaria {

/// What the compositor is willing to be told about. Everything not listed makes
/// the client's request a protocol error, so clients never send what cannot be honoured.
struct ColorSupport {
    std::vector<Primaries> primaries;
    std::vector<TransferFunction> transfer_functions;
    std::vector<RenderIntent> render_intents = {RenderIntent::perceptual};
    bool set_primaries = false;                 ///< custom chromaticities
    bool set_tf_power = false;                  ///< pure power-law transfer function
    bool set_luminances = false;
    bool set_mastering_display_primaries = false;
    bool extended_target_volume = false;

    /// sRGB, and only sRGB: the curves that are, or are within a hair of, sRGB's.
    /// What a compositor with no colour pipeline should advertise.
    [[nodiscard]] static ColorSupport srgb_only() {
        ColorSupport s;
        s.primaries = {Primaries::srgb};
        s.transfer_functions = {TransferFunction::compound_power_2_4, TransferFunction::srgb,
                                TransferFunction::gamma22};
        s.set_luminances = true; // an SDR client may state its 80-nit white
        return s;
    }
    /// Every named primary and transfer function, and every parametric feature:
    /// for a compositor whose renderer converts between them.
    [[nodiscard]] static ColorSupport full() {
        ColorSupport s;
        s.primaries = {Primaries::srgb,        Primaries::pal_m,     Primaries::pal,
                       Primaries::ntsc,        Primaries::generic_film, Primaries::bt2020,
                       Primaries::cie1931_xyz, Primaries::dci_p3,    Primaries::display_p3,
                       Primaries::adobe_rgb};
        s.transfer_functions = {TransferFunction::bt1886,   TransferFunction::gamma22,
                                TransferFunction::gamma28,  TransferFunction::st240,
                                TransferFunction::ext_linear, TransferFunction::log_100,
                                TransferFunction::log_316,  TransferFunction::xvycc,
                                TransferFunction::srgb,     TransferFunction::st2084_pq,
                                TransferFunction::st428,    TransferFunction::hlg,
                                TransferFunction::compound_power_2_4};
        s.render_intents = {RenderIntent::perceptual, RenderIntent::relative,
                            RenderIntent::saturation, RenderIntent::absolute,
                            RenderIntent::relative_bpc};
        s.set_primaries = s.set_tf_power = s.set_luminances = true;
        s.set_mastering_display_primaries = true;
        return s;
    }
};

/// The wp_color_manager_v1 global (version 3). Move-only; pointer-stable state.
class ColorManager {
public:
    [[nodiscard]] static Result<ColorManager> create(Display& display,
                                                     ColorSupport support = ColorSupport::srgb_only());

    ~ColorManager();
    ColorManager(ColorManager&&) noexcept;
    ColorManager& operator=(ColorManager&&) noexcept;
    ColorManager(const ColorManager&) = delete;
    ColorManager& operator=(const ColorManager&) = delete;

    /// What `output` is: the colour space a client should render for to look
    /// right on it. Clients that already asked are told it changed. Outputs
    /// default to sRGB.
    void set_output_description(const OutputGlobal& output, ImageDescription description);
    /// The description of `output` as set, or sRGB.
    [[nodiscard]] ImageDescription output_description(const OutputGlobal& output) const;

    /// What a surface should render in, by default, for any surface that has no
    /// preferred description of its own. Usually sRGB.
    void set_default_preferred(ImageDescription description);
    /// The colour space `surface` would be best rendered in — typically its
    /// output's description. Clients watching its feedback object are told.
    void set_preferred(const Surface& surface, ImageDescription description);

    /// The description clients have asked to be shown through, how many.
    [[nodiscard]] const ColorSupport& support() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit ColorManager(std::unique_ptr<Impl> impl) noexcept;
};

/// What `ColorRepresentationManager` accepts. A compositor whose renderer cannot
/// read YUV should leave this at the default, so video clients convert to RGB
/// themselves.
struct ColorRepresentationSupport {
    std::vector<AlphaMode> alpha_modes = {AlphaMode::premultiplied_electrical};
    /// Combinations of matrix and range. Identity + full is plain RGB.
    std::vector<std::pair<Coefficients, ColorRange>> coefficients = {
        {Coefficients::identity, ColorRange::full}};
};

/// The wp_color_representation_manager_v1 global (version 1). Move-only.
class ColorRepresentationManager {
public:
    [[nodiscard]] static Result<ColorRepresentationManager> create(
        Display& display, ColorRepresentationSupport support = {});

    ~ColorRepresentationManager();
    ColorRepresentationManager(ColorRepresentationManager&&) noexcept;
    ColorRepresentationManager& operator=(ColorRepresentationManager&&) noexcept;
    ColorRepresentationManager(const ColorRepresentationManager&) = delete;
    ColorRepresentationManager& operator=(const ColorRepresentationManager&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit ColorRepresentationManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation

namespace luminaria {

// Not in an anonymous namespace: the Impls hold pointers to these. Unexported.
struct CmOutputObject;
struct CmSurfaceObject;
struct CmFeedbackObject;

struct ColorManager::Impl {
    WlGlobal global;
    ColorSupport support;
    std::uint64_t next_identity = 1;
    ImageDescription default_preferred;
    /// Every description made so far that something still holds, so equal
    /// spaces share one identity.
    std::vector<std::weak_ptr<const ImageDescription>> known;
    std::map<const void*, std::shared_ptr<const ImageDescription>> outputs;
    std::vector<std::pair<SurfaceId, std::shared_ptr<const ImageDescription>>> preferred;
    std::vector<CmOutputObject*> output_objects;
    std::vector<CmSurfaceObject*> surface_objects;
    std::vector<CmFeedbackObject*> feedback_objects;

    /// The shared description equal to `d`, with an identity.
    std::shared_ptr<const ImageDescription> intern(ImageDescription d) {
        std::erase_if(known, [](const auto& w) { return w.expired(); });
        for (const auto& w : known) {
            if (auto existing = w.lock(); existing != nullptr && existing->same_space(d)) {
                return existing;
            }
        }
        d.identity = next_identity++;
        auto fresh = std::make_shared<const ImageDescription>(std::move(d));
        known.push_back(fresh);
        return fresh;
    }
    std::shared_ptr<const ImageDescription> preferred_for(SurfaceId id) {
        for (const auto& [surface, d] : preferred) {
            if (surface == id) {
                return d;
            }
        }
        return intern(default_preferred);
    }

    ~Impl();
};

struct CmOutputObject {
    ColorManager::Impl* impl = nullptr;
    const void* output = nullptr;
    wl_resource* resource = nullptr;
};
struct CmSurfaceObject {
    ColorManager::Impl* impl = nullptr;
    SurfaceId surface;
};
struct CmFeedbackObject {
    ColorManager::Impl* impl = nullptr;
    SurfaceId surface;
    wl_resource* resource = nullptr;
};

namespace {

using CmImpl = ColorManager::Impl;

constexpr double kLumScale = 10000.0;
constexpr double kChroma = 1000000.0;

wl_resource* make_resource(wl_client* client, const wl_interface* iface, std::uint32_t version,
                           std::uint32_t id) {
    wl_resource* r = wl_resource_create(client, iface, static_cast<int>(version), id);
    if (r == nullptr) {
        wl_client_post_no_memory(client);
    }
    return r;
}

// ---- wp_image_description_v1 -------------------------------------------

struct CmImageDescription {
    std::shared_ptr<const ImageDescription> description;
    bool queryable = false; // compositor-made descriptions only
};

CmImageDescription* cm_image_of(wl_resource* r) {
    return static_cast<CmImageDescription*>(wl_resource_get_user_data(r));
}

std::int32_t chroma(double v) { return static_cast<std::int32_t>(std::lround(v * kChroma)); }

void send_primaries(wl_resource* info, const PrimarySet& p, bool target) {
    const auto send = target ? wp_image_description_info_v1_send_target_primaries
                             : wp_image_description_info_v1_send_primaries;
    send(info, chroma(p.red.x), chroma(p.red.y), chroma(p.green.x), chroma(p.green.y),
         chroma(p.blue.x), chroma(p.blue.y), chroma(p.white.x), chroma(p.white.y));
}

void cm_image_get_information(wl_client* client, wl_resource* resource, std::uint32_t id) {
    CmImageDescription* image = cm_image_of(resource);
    if (!image->queryable) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION,
                               "this image description was made by a client and cannot be queried");
        return;
    }
    wl_resource* info = make_resource(client, &wp_image_description_info_v1_interface,
                                      wl_resource_get_version(resource), id);
    if (info == nullptr) {
        return;
    }
    wl_resource_set_implementation(info, nullptr, nullptr, nullptr);
    const ImageDescription& d = *image->description;
    send_primaries(info, d.primaries, false);
    if (d.named_primaries.has_value()) {
        wp_image_description_info_v1_send_primaries_named(
            info, static_cast<std::uint32_t>(*d.named_primaries));
    }
    if (d.transfer_function.has_value()) {
        wp_image_description_info_v1_send_tf_named(info,
                                                   static_cast<std::uint32_t>(*d.transfer_function));
    } else {
        wp_image_description_info_v1_send_tf_power(
            info, static_cast<std::uint32_t>(std::lround(d.power * kLumScale)));
    }
    wp_image_description_info_v1_send_luminances(
        info, static_cast<std::uint32_t>(std::lround(d.min_luminance * kLumScale)),
        static_cast<std::uint32_t>(std::lround(d.max_luminance)),
        static_cast<std::uint32_t>(std::lround(d.reference_luminance)));
    send_primaries(info, d.target_primaries.value_or(d.primaries), true);
    const auto [tmin, tmax] =
        d.target_luminance.value_or(std::pair{d.min_luminance, d.max_luminance});
    wp_image_description_info_v1_send_target_luminance(
        info, static_cast<std::uint32_t>(std::lround(tmin * kLumScale)),
        static_cast<std::uint32_t>(std::lround(tmax)));
    if (d.max_cll.has_value()) {
        wp_image_description_info_v1_send_target_max_cll(info, *d.max_cll);
    }
    if (d.max_fall.has_value()) {
        wp_image_description_info_v1_send_target_max_fall(info, *d.max_fall);
    }
    // `done` is a destructor event: it ends the object on the wire.
    wp_image_description_info_v1_send_done(info);
    wl_resource_destroy(info);
}

constexpr struct wp_image_description_v1_interface cm_image_impl = {
    .destroy = resource_destroy_request,
    .get_information = cm_image_get_information,
};

void cm_image_destroyed(wl_resource* r) { delete cm_image_of(r); }

/// A ready image description for `description`, sent to `client` as `id`.
wl_resource* make_image_description(wl_client* client, std::uint32_t version, std::uint32_t id,
                                    std::shared_ptr<const ImageDescription> description,
                                    bool queryable) {
    wl_resource* r = make_resource(client, &wp_image_description_v1_interface, version, id);
    if (r == nullptr) {
        return nullptr;
    }
    const std::uint64_t identity = description->identity;
    wl_resource_set_implementation(r, &cm_image_impl,
                                   new CmImageDescription{std::move(description), queryable},
                                   cm_image_destroyed);
    if (version >= WP_IMAGE_DESCRIPTION_V1_READY2_SINCE_VERSION) {
        wp_image_description_v1_send_ready2(r, static_cast<std::uint32_t>(identity >> 32),
                                            static_cast<std::uint32_t>(identity & 0xffffffffu));
    } else {
        wp_image_description_v1_send_ready(r, static_cast<std::uint32_t>(identity));
    }
    return r;
}

// ---- wp_color_management_output_v1 -------------------------------------

CmOutputObject* cm_output_of(wl_resource* r) {
    return static_cast<CmOutputObject*>(wl_resource_get_user_data(r));
}

void cm_output_get_image_description(wl_client* client, wl_resource* resource, std::uint32_t id) {
    CmOutputObject* o = cm_output_of(resource);
    std::shared_ptr<const ImageDescription> d;
    if (o->impl != nullptr) {
        if (auto it = o->impl->outputs.find(o->output); it != o->impl->outputs.end()) {
            d = it->second;
        } else {
            d = o->impl->intern(ImageDescription::srgb());
        }
    } else {
        d = std::make_shared<const ImageDescription>(ImageDescription::srgb());
    }
    make_image_description(client, wl_resource_get_version(resource), id, std::move(d), true);
}

constexpr struct wp_color_management_output_v1_interface cm_output_impl = {
    .destroy = resource_destroy_request,
    .get_image_description = cm_output_get_image_description,
};

void cm_output_destroyed(wl_resource* r) {
    CmOutputObject* o = cm_output_of(r);
    if (o->impl != nullptr) {
        std::erase(o->impl->output_objects, o);
    }
    delete o;
}

// ---- wp_color_management_surface_v1 ------------------------------------

CmSurfaceObject* cm_surface_of(wl_resource* r) {
    return static_cast<CmSurfaceObject*>(wl_resource_get_user_data(r));
}

bool intent_supported(const CmImpl& impl, std::uint32_t intent) {
    return std::ranges::any_of(impl.support.render_intents, [intent](RenderIntent i) {
        return static_cast<std::uint32_t>(i) == intent;
    });
}

void cm_surface_set_image_description(wl_client*, wl_resource* resource, wl_resource* description,
                                   std::uint32_t intent) {
    CmSurfaceObject* o = cm_surface_of(resource);
    Surface* surface = surface_from_id(o->surface);
    if (surface == nullptr) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
                               "the wl_surface is gone");
        return;
    }
    if (o->impl == nullptr || !intent_supported(*o->impl, intent)) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT,
                               "render intent %u is not supported", intent);
        return;
    }
    surface->set_pending_image_description(cm_image_of(description)->description,
                                           static_cast<RenderIntent>(intent));
}

void cm_surface_unset_image_description(wl_client*, wl_resource* resource) {
    CmSurfaceObject* o = cm_surface_of(resource);
    Surface* surface = surface_from_id(o->surface);
    if (surface == nullptr) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
                               "the wl_surface is gone");
        return;
    }
    surface->set_pending_image_description(nullptr, RenderIntent::perceptual);
}

constexpr struct wp_color_management_surface_v1_interface cm_surface_impl = {
    .destroy = resource_destroy_request,
    .set_image_description = cm_surface_set_image_description,
    .unset_image_description = cm_surface_unset_image_description,
};

void cm_surface_destroyed(wl_resource* r) {
    CmSurfaceObject* o = cm_surface_of(r);
    if (o->impl != nullptr) {
        std::erase(o->impl->surface_objects, o);
    }
    // Destroying the extension object drops the description, at the next commit.
    if (Surface* surface = surface_from_id(o->surface); surface != nullptr && o->impl != nullptr) {
        surface->set_pending_image_description(nullptr, RenderIntent::perceptual);
    }
    delete o;
}

// ---- wp_color_management_surface_feedback_v1 ---------------------------

CmFeedbackObject* cm_feedback_of(wl_resource* r) {
    return static_cast<CmFeedbackObject*>(wl_resource_get_user_data(r));
}

void cm_feedback_get_preferred(wl_client* client, wl_resource* resource, std::uint32_t id) {
    CmFeedbackObject* o = cm_feedback_of(resource);
    if (o->impl == nullptr || surface_from_id(o->surface) == nullptr) {
        wl_resource_post_error(resource, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT,
                               "the wl_surface is gone");
        return;
    }
    make_image_description(client, wl_resource_get_version(resource), id,
                           o->impl->preferred_for(o->surface), true);
}

constexpr struct wp_color_management_surface_feedback_v1_interface cm_feedback_impl = {
    .destroy = resource_destroy_request,
    .get_preferred = cm_feedback_get_preferred,
    .get_preferred_parametric = cm_feedback_get_preferred, // every description is parametric
};

void cm_feedback_destroyed(wl_resource* r) {
    CmFeedbackObject* o = cm_feedback_of(r);
    if (o->impl != nullptr) {
        std::erase(o->impl->feedback_objects, o);
    }
    delete o;
}

void notify_preferred(const CmFeedbackObject& f, const ImageDescription& d) {
    const std::uint32_t version = wl_resource_get_version(f.resource);
    if (version >= WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_PREFERRED_CHANGED2_SINCE_VERSION) {
        wp_color_management_surface_feedback_v1_send_preferred_changed2(
            f.resource, static_cast<std::uint32_t>(d.identity >> 32),
            static_cast<std::uint32_t>(d.identity & 0xffffffffu));
    } else {
        wp_color_management_surface_feedback_v1_send_preferred_changed(
            f.resource, static_cast<std::uint32_t>(d.identity));
    }
}

// ---- wp_image_description_creator_params_v1 ----------------------------

struct CmCreator {
    CmImpl* impl = nullptr;
    std::optional<TransferFunction> tf;
    std::optional<double> power;
    bool tf_set = false;
    std::optional<Primaries> named_primaries;
    std::optional<PrimarySet> primaries;
    bool primaries_set = false;
    std::optional<std::array<double, 3>> luminances; // min, max, reference
    std::optional<PrimarySet> mastering_primaries;
    std::optional<std::pair<double, double>> mastering_luminance;
    std::optional<std::uint32_t> max_cll, max_fall;
};

CmCreator* cm_creator_of(wl_resource* r) {
    return static_cast<CmCreator*>(wl_resource_get_user_data(r));
}

bool supports_tf(const CmImpl& impl, std::uint32_t tf) {
    return std::ranges::any_of(impl.support.transfer_functions, [tf](TransferFunction t) {
        return static_cast<std::uint32_t>(t) == tf;
    });
}
bool supports_primaries(const CmImpl& impl, std::uint32_t p) {
    return std::ranges::any_of(impl.support.primaries,
                               [p](Primaries q) { return static_cast<std::uint32_t>(q) == p; });
}

/// False, with the protocol error posted, if the feature is not on offer.
bool require(wl_resource* resource, bool feature, const char* name) {
    if (!feature) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
                               "%s is not supported", name);
    }
    return feature;
}

bool already(wl_resource* resource, bool set, const char* what) {
    if (set) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                               "%s was already set", what);
    }
    return set;
}

void cm_params_set_tf_named(wl_client*, wl_resource* resource, std::uint32_t tf) {
    CmCreator* c = cm_creator_of(resource);
    if (already(resource, c->tf_set, "the transfer function")) {
        return;
    }
    if (c->impl == nullptr || !supports_tf(*c->impl, tf)) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
                               "transfer function %u is not supported", tf);
        return;
    }
    c->tf = static_cast<TransferFunction>(tf);
    c->tf_set = true;
}

void cm_params_set_tf_power(wl_client*, wl_resource* resource, std::uint32_t eexp) {
    CmCreator* c = cm_creator_of(resource);
    if (already(resource, c->tf_set, "the transfer function") ||
        !require(resource, c->impl != nullptr && c->impl->support.set_tf_power, "set_tf_power")) {
        return;
    }
    if (eexp < 10000 || eexp > 100000) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
                               "the exponent must be between 1.0 and 10.0");
        return;
    }
    c->power = eexp / kLumScale;
    c->tf_set = true;
}

void cm_params_set_primaries_named(wl_client*, wl_resource* resource, std::uint32_t primaries) {
    CmCreator* c = cm_creator_of(resource);
    if (already(resource, c->primaries_set, "the primaries")) {
        return;
    }
    if (c->impl == nullptr || !supports_primaries(*c->impl, primaries)) {
        wl_resource_post_error(resource,
                               WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
                               "primaries %u are not supported", primaries);
        return;
    }
    c->named_primaries = static_cast<Primaries>(primaries);
    c->primaries = primaries_of(*c->named_primaries);
    c->primaries_set = true;
}

PrimarySet to_primaries(std::int32_t rx, std::int32_t ry, std::int32_t gx, std::int32_t gy,
                        std::int32_t bx, std::int32_t by, std::int32_t wx, std::int32_t wy) {
    const auto c = [](std::int32_t v) { return v / kChroma; };
    return {{c(rx), c(ry)}, {c(gx), c(gy)}, {c(bx), c(by)}, {c(wx), c(wy)}};
}

void cm_params_set_primaries(wl_client*, wl_resource* resource, std::int32_t rx, std::int32_t ry,
                          std::int32_t gx, std::int32_t gy, std::int32_t bx, std::int32_t by,
                          std::int32_t wx, std::int32_t wy) {
    CmCreator* c = cm_creator_of(resource);
    if (already(resource, c->primaries_set, "the primaries") ||
        !require(resource, c->impl != nullptr && c->impl->support.set_primaries, "set_primaries")) {
        return;
    }
    c->primaries = to_primaries(rx, ry, gx, gy, bx, by, wx, wy);
    c->named_primaries.reset();
    // Custom chromaticities that spell a named set are that set.
    for (std::uint32_t p = 1; p <= 10; ++p) {
        if (primaries_of(static_cast<Primaries>(p)) == *c->primaries) {
            c->named_primaries = static_cast<Primaries>(p);
        }
    }
    c->primaries_set = true;
}

void cm_params_set_luminances(wl_client*, wl_resource* resource, std::uint32_t min_lum,
                           std::uint32_t max_lum, std::uint32_t reference_lum) {
    CmCreator* c = cm_creator_of(resource);
    if (already(resource, c->luminances.has_value(), "the luminances") ||
        !require(resource, c->impl != nullptr && c->impl->support.set_luminances,
                 "set_luminances")) {
        return;
    }
    const double min = min_lum / kLumScale;
    if (!(max_lum > min) || !(reference_lum > min)) {
        wl_resource_post_error(resource,
                               WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                               "max and reference luminance must exceed the minimum");
        return;
    }
    c->luminances = std::array<double, 3>{min, static_cast<double>(max_lum),
                                          static_cast<double>(reference_lum)};
}

void cm_params_set_mastering_primaries(wl_client*, wl_resource* resource, std::int32_t rx,
                                    std::int32_t ry, std::int32_t gx, std::int32_t gy,
                                    std::int32_t bx, std::int32_t by, std::int32_t wx,
                                    std::int32_t wy) {
    CmCreator* c = cm_creator_of(resource);
    if (already(resource, c->mastering_primaries.has_value(), "the mastering primaries") ||
        !require(resource,
                 c->impl != nullptr && c->impl->support.set_mastering_display_primaries,
                 "set_mastering_display_primaries")) {
        return;
    }
    c->mastering_primaries = to_primaries(rx, ry, gx, gy, bx, by, wx, wy);
}

void cm_params_set_mastering_luminance(wl_client*, wl_resource* resource, std::uint32_t min_lum,
                                    std::uint32_t max_lum) {
    CmCreator* c = cm_creator_of(resource);
    if (already(resource, c->mastering_luminance.has_value(), "the mastering luminance") ||
        !require(resource,
                 c->impl != nullptr && c->impl->support.set_mastering_display_primaries,
                 "set_mastering_display_primaries")) {
        return;
    }
    const double min = min_lum / kLumScale;
    if (!(max_lum > min)) {
        wl_resource_post_error(resource,
                               WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                               "max luminance must exceed the minimum");
        return;
    }
    c->mastering_luminance = std::pair{min, static_cast<double>(max_lum)};
}

void cm_params_set_max_cll(wl_client*, wl_resource* resource, std::uint32_t value) {
    cm_creator_of(resource)->max_cll = value;
}
void cm_params_set_max_fall(wl_client*, wl_resource* resource, std::uint32_t value) {
    cm_creator_of(resource)->max_fall = value;
}

void cm_params_create(wl_client* client, wl_resource* resource, std::uint32_t id) {
    CmCreator* c = cm_creator_of(resource);
    if (!c->tf_set || !c->primaries_set) {
        wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
                               "both the transfer function and the primaries are required");
        return;
    }
    ImageDescription d = ImageDescription::with_defaults(c->tf);
    d.primaries = *c->primaries;
    d.named_primaries = c->named_primaries;
    d.transfer_function = c->tf;
    d.power = c->power.value_or(0.0);
    if (c->luminances.has_value()) {
        d.min_luminance = (*c->luminances)[0];
        d.max_luminance = (*c->luminances)[1];
        d.reference_luminance = (*c->luminances)[2];
        if (c->tf == TransferFunction::st2084_pq) {
            d.max_luminance = d.min_luminance + 10000.0; // PQ's range is fixed
        }
    }
    d.target_primaries = c->mastering_primaries;
    d.target_luminance = c->mastering_luminance;
    d.max_cll = c->max_cll;
    d.max_fall = c->max_fall;
    auto shared = c->impl != nullptr ? c->impl->intern(std::move(d))
                                     : std::make_shared<const ImageDescription>(std::move(d));
    // Client-made: usable on surfaces, but the client cannot read it back.
    make_image_description(client, wl_resource_get_version(resource), id, std::move(shared), false);
    // `create` is a destructor request: the creator is spent.
    wl_resource_destroy(resource);
}

constexpr struct wp_image_description_creator_params_v1_interface cm_params_impl = {
    .create = cm_params_create,
    .set_tf_named = cm_params_set_tf_named,
    .set_tf_power = cm_params_set_tf_power,
    .set_primaries_named = cm_params_set_primaries_named,
    .set_primaries = cm_params_set_primaries,
    .set_luminances = cm_params_set_luminances,
    .set_mastering_display_primaries = cm_params_set_mastering_primaries,
    .set_mastering_luminance = cm_params_set_mastering_luminance,
    .set_max_cll = cm_params_set_max_cll,
    .set_max_fall = cm_params_set_max_fall,
};

void cm_creator_destroyed(wl_resource* r) { delete cm_creator_of(r); }

// ---- wp_color_manager_v1 ---------------------------------------------------

CmImpl* cm_impl_of(wl_resource* r) {
    return static_cast<CmImpl*>(wl_resource_get_user_data(r));
}

void cm_manager_get_output(wl_client* client, wl_resource* manager, std::uint32_t id,
                        wl_resource* wl_output) {
    CmImpl* impl = cm_impl_of(manager);
    wl_resource* r = make_resource(client, &wp_color_management_output_v1_interface,
                                   wl_resource_get_version(manager), id);
    if (r == nullptr) {
        return;
    }
    auto* o = new CmOutputObject{impl, OutputGlobal::identity_of(wl_output), r};
    wl_resource_set_implementation(r, &cm_output_impl, o, cm_output_destroyed);
    if (impl != nullptr) {
        impl->output_objects.push_back(o);
    }
}

void cm_manager_get_surface(wl_client* client, wl_resource* manager, std::uint32_t id,
                         wl_resource* surface_resource) {
    CmImpl* impl = cm_impl_of(manager);
    Surface* surface = surface_from_resource(surface_resource);
    if (impl != nullptr && surface != nullptr &&
        std::ranges::any_of(impl->surface_objects,
                            [&](const CmSurfaceObject* o) { return o->surface == surface->id(); })) {
        wl_resource_post_error(manager, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
                               "this wl_surface already has a wp_color_management_surface_v1");
        return;
    }
    wl_resource* r = make_resource(client, &wp_color_management_surface_v1_interface,
                                   wl_resource_get_version(manager), id);
    if (r == nullptr) {
        return;
    }
    auto* o = new CmSurfaceObject{impl, surface != nullptr ? surface->id() : SurfaceId{}};
    wl_resource_set_implementation(r, &cm_surface_impl, o, cm_surface_destroyed);
    if (impl != nullptr) {
        impl->surface_objects.push_back(o);
    }
}

void cm_manager_get_surface_feedback(wl_client* client, wl_resource* manager, std::uint32_t id,
                                  wl_resource* surface_resource) {
    CmImpl* impl = cm_impl_of(manager);
    Surface* surface = surface_from_resource(surface_resource);
    wl_resource* r = make_resource(client, &wp_color_management_surface_feedback_v1_interface,
                                   wl_resource_get_version(manager), id);
    if (r == nullptr) {
        return;
    }
    auto* o = new CmFeedbackObject{impl, surface != nullptr ? surface->id() : SurfaceId{}, r};
    wl_resource_set_implementation(r, &cm_feedback_impl, o, cm_feedback_destroyed);
    if (impl != nullptr) {
        impl->feedback_objects.push_back(o);
    }
}

void cm_manager_create_icc_creator(wl_client*, wl_resource* manager, std::uint32_t) {
    wl_resource_post_error(manager, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
                           "ICC profiles are not supported");
}

void cm_manager_create_parametric_creator(wl_client* client, wl_resource* manager, std::uint32_t id) {
    wl_resource* r = make_resource(client, &wp_image_description_creator_params_v1_interface,
                                   wl_resource_get_version(manager), id);
    if (r == nullptr) {
        return;
    }
    auto* c = new CmCreator{};
    c->impl = cm_impl_of(manager);
    wl_resource_set_implementation(r, &cm_params_impl, c, cm_creator_destroyed);
}

void cm_manager_create_windows_scrgb(wl_client*, wl_resource* manager, std::uint32_t) {
    wl_resource_post_error(manager, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
                           "windows_scrgb is not supported");
}

// A reference is a name handed over by another protocol; nothing in this
// library creates one, so a client cannot have one to give.
void cm_manager_get_image_description(wl_client*, wl_resource* manager, std::uint32_t,
                                   wl_resource*) {
    wl_resource_post_error(manager, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
                           "image description references are not supported");
}

void cm_manager_create_windows_bt2100(wl_client*, wl_resource* manager, std::uint32_t) {
    wl_resource_post_error(manager, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
                           "windows_bt2100 is not supported");
}

constexpr struct wp_color_manager_v1_interface cm_manager_impl = {
    .destroy = resource_destroy_request,
    .get_output = cm_manager_get_output,
    .get_surface = cm_manager_get_surface,
    .get_surface_feedback = cm_manager_get_surface_feedback,
    .create_icc_creator = cm_manager_create_icc_creator,
    .create_parametric_creator = cm_manager_create_parametric_creator,
    .create_windows_scrgb = cm_manager_create_windows_scrgb,
    .get_image_description = cm_manager_get_image_description,
    .create_windows_bt2100 = cm_manager_create_windows_bt2100,
};

void cm_manager_bind(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
    wl_resource* resource = make_resource(client, &wp_color_manager_v1_interface, version, id);
    if (resource == nullptr) {
        return;
    }
    wl_resource_set_implementation(resource, &cm_manager_impl, data, nullptr);
    auto* impl = static_cast<CmImpl*>(data);
    if (impl == nullptr) {
        return;
    }
    const ColorSupport& s = impl->support;
    for (RenderIntent i : s.render_intents) {
        wp_color_manager_v1_send_supported_intent(resource, static_cast<std::uint32_t>(i));
    }
    wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC);
    if (s.set_primaries) {
        wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES);
    }
    if (s.set_tf_power) {
        wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER);
    }
    if (s.set_luminances) {
        wp_color_manager_v1_send_supported_feature(resource, WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES);
    }
    if (s.set_mastering_display_primaries) {
        wp_color_manager_v1_send_supported_feature(
            resource, WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES);
    }
    if (s.extended_target_volume) {
        wp_color_manager_v1_send_supported_feature(
            resource, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME);
    }
    for (TransferFunction tf : s.transfer_functions) {
        // The two sRGB entries were deprecated in v2 in favour of compound_power_2_4.
        if (version >= 2 && (tf == TransferFunction::srgb || tf == TransferFunction::ext_srgb)) {
            continue;
        }
        if (version < 2 && tf == TransferFunction::compound_power_2_4) {
            continue;
        }
        wp_color_manager_v1_send_supported_tf_named(resource, static_cast<std::uint32_t>(tf));
    }
    for (Primaries p : s.primaries) {
        wp_color_manager_v1_send_supported_primaries_named(resource, static_cast<std::uint32_t>(p));
    }
    wp_color_manager_v1_send_done(resource);
}

} // namespace

ColorManager::Impl::~Impl() {
    // The objects are the client's and outlive us at exit (the Display, declared
    // first, is what destroys a still-connected client). Leave them inert.
    for (CmOutputObject* o : output_objects) {
        o->impl = nullptr;
    }
    for (CmSurfaceObject* o : surface_objects) {
        o->impl = nullptr;
    }
    for (CmFeedbackObject* o : feedback_objects) {
        o->impl = nullptr;
    }
}

ColorManager::ColorManager(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ColorManager::~ColorManager() = default;
ColorManager::ColorManager(ColorManager&&) noexcept = default;
ColorManager& ColorManager::operator=(ColorManager&&) noexcept = default;

Result<ColorManager> ColorManager::create(Display& display, ColorSupport support) {
    auto impl = std::make_unique<Impl>();
    impl->support = std::move(support);
    auto global = create_wl_global<&wp_color_manager_v1_interface, cm_manager_bind>(display, 3, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return ColorManager{std::move(impl)};
}

const ColorSupport& ColorManager::support() const noexcept { return impl_->support; }

void ColorManager::set_output_description(const OutputGlobal& output, ImageDescription description) {
    auto now = impl_->intern(std::move(description));
    auto& slot = impl_->outputs[output.identity()];
    if (slot != nullptr && slot->same_space(*now)) {
        return;
    }
    slot = std::move(now);
    const std::vector<CmOutputObject*> live = impl_->output_objects;
    for (CmOutputObject* o : live) {
        if (o->output == output.identity()) {
            wp_color_management_output_v1_send_image_description_changed(o->resource);
        }
    }
}

ImageDescription ColorManager::output_description(const OutputGlobal& output) const {
    auto it = impl_->outputs.find(output.identity());
    return it != impl_->outputs.end() ? *it->second : ImageDescription::srgb();
}

void ColorManager::set_default_preferred(ImageDescription description) {
    impl_->default_preferred = std::move(description);
}

void ColorManager::set_preferred(const Surface& surface, ImageDescription description) {
    auto now = impl_->intern(std::move(description));
    const SurfaceId id = surface.id();
    auto it = std::ranges::find_if(impl_->preferred, [&](const auto& e) { return e.first == id; });
    if (it != impl_->preferred.end()) {
        if (it->second->same_space(*now)) {
            return;
        }
        it->second = now;
    } else {
        // A surface that never had one has been told the default; only a
        // real change is news.
        const bool news = !impl_->intern(impl_->default_preferred)->same_space(*now);
        impl_->preferred.emplace_back(id, now);
        if (!news) {
            return;
        }
    }
    const std::vector<CmFeedbackObject*> live = impl_->feedback_objects;
    for (CmFeedbackObject* f : live) {
        if (f->surface == id) {
            notify_preferred(*f, *now);
        }
    }
}

// ============================================================ color representation

// Not in an anonymous namespace: the Impl holds pointers to it.
struct CrSurface {
    ColorRepresentationManager::Impl* impl = nullptr;
    SurfaceId surface;
    ColorRepresentation state;
};

struct ColorRepresentationManager::Impl {
    WlGlobal global;
    ColorRepresentationSupport support;
    std::vector<CrSurface*> objects; // live, one per surface that has one

    ~Impl() {
        // The objects are the client's and outlive us at exit. Leave them inert.
        for (CrSurface* o : objects) {
            o->impl = nullptr;
        }
    }
};

namespace {

using CrImpl = ColorRepresentationManager::Impl;

CrSurface* cr_surface_of(wl_resource* r) { return static_cast<CrSurface*>(wl_resource_get_user_data(r)); }

void cr_apply(CrSurface& s) {
    if (Surface* surface = surface_from_id(s.surface); surface != nullptr) {
        surface->set_pending_color_representation(s.state);
    }
}

bool cr_inert(wl_resource* resource, CrSurface& s) {
    if (surface_from_id(s.surface) == nullptr) {
        wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
                               "the wl_surface is gone");
        return true;
    }
    return false;
}

void cr_set_alpha_mode(wl_client*, wl_resource* resource, std::uint32_t mode) {
    CrSurface* s = cr_surface_of(resource);
    if (cr_inert(resource, *s)) {
        return;
    }
    if (s->impl == nullptr || !std::ranges::any_of(s->impl->support.alpha_modes, [mode](AlphaMode m) {
            return static_cast<std::uint32_t>(m) == mode;
        })) {
        wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE,
                               "alpha mode %u is not supported", mode);
        return;
    }
    s->state.alpha_mode = static_cast<AlphaMode>(mode);
    cr_apply(*s);
}

void cr_set_coefficients_and_range(wl_client*, wl_resource* resource, std::uint32_t coefficients,
                                   std::uint32_t range) {
    CrSurface* s = cr_surface_of(resource);
    if (cr_inert(resource, *s)) {
        return;
    }
    if (s->impl == nullptr ||
        !std::ranges::any_of(s->impl->support.coefficients, [&](const auto& pair) {
            return static_cast<std::uint32_t>(pair.first) == coefficients &&
                   static_cast<std::uint32_t>(pair.second) == range;
        })) {
        wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
                               "coefficients %u with range %u are not supported", coefficients, range);
        return;
    }
    s->state.coefficients = static_cast<Coefficients>(coefficients);
    s->state.range = static_cast<ColorRange>(range);
    cr_apply(*s);
}

void cr_set_chroma_location(wl_client*, wl_resource* resource, std::uint32_t location) {
    CrSurface* s = cr_surface_of(resource);
    if (cr_inert(resource, *s)) {
        return;
    }
    if (location < 1 || location > 6) {
        wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_CHROMA_LOCATION,
                               "chroma location %u is not defined", location);
        return;
    }
    s->state.chroma_location = static_cast<ChromaLocation>(location);
    cr_apply(*s);
}

constexpr struct wp_color_representation_surface_v1_interface cr_surface_impl = {
    .destroy = resource_destroy_request,
    .set_alpha_mode = cr_set_alpha_mode,
    .set_coefficients_and_range = cr_set_coefficients_and_range,
    .set_chroma_location = cr_set_chroma_location,
};

void cr_surface_destroyed(wl_resource* r) {
    CrSurface* s = cr_surface_of(r);
    if (s->impl != nullptr) {
        std::erase(s->impl->objects, s);
    }
    // Destroying the object removes what it set, at the next commit.
    if (surface_from_id(s->surface) != nullptr && s->impl != nullptr) {
        s->state = {};
        cr_apply(*s);
    }
    delete s;
}

void cr_manager_get_surface(wl_client* client, wl_resource* manager, std::uint32_t id,
                            wl_resource* surface_resource) {
    auto* impl = static_cast<CrImpl*>(wl_resource_get_user_data(manager));
    Surface* surface = surface_from_resource(surface_resource);
    if (impl != nullptr && surface != nullptr &&
        std::ranges::any_of(impl->objects,
                            [&](const CrSurface* o) { return o->surface == surface->id(); })) {
        wl_resource_post_error(manager, WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS,
                               "this wl_surface already has a wp_color_representation_surface_v1");
        return;
    }
    wl_resource* r = make_resource(client, &wp_color_representation_surface_v1_interface,
                                   wl_resource_get_version(manager), id);
    if (r == nullptr) {
        return;
    }
    auto* s = new CrSurface{impl, surface != nullptr ? surface->id() : SurfaceId{}, {}};
    wl_resource_set_implementation(r, &cr_surface_impl, s, cr_surface_destroyed);
    if (impl != nullptr) {
        impl->objects.push_back(s);
    }
}

constexpr struct wp_color_representation_manager_v1_interface cr_manager_impl = {
    .destroy = resource_destroy_request,
    .get_surface = cr_manager_get_surface,
};

void cr_manager_bind(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
    wl_resource* resource =
        make_resource(client, &wp_color_representation_manager_v1_interface, version, id);
    if (resource == nullptr) {
        return;
    }
    wl_resource_set_implementation(resource, &cr_manager_impl, data, nullptr);
    auto* impl = static_cast<CrImpl*>(data);
    if (impl == nullptr) {
        return;
    }
    for (AlphaMode m : impl->support.alpha_modes) {
        wp_color_representation_manager_v1_send_supported_alpha_mode(resource,
                                                                     static_cast<std::uint32_t>(m));
    }
    for (const auto& [c, r] : impl->support.coefficients) {
        wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
            resource, static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(r));
    }
    wp_color_representation_manager_v1_send_done(resource);
}

} // namespace

ColorRepresentationManager::ColorRepresentationManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
ColorRepresentationManager::~ColorRepresentationManager() = default;
ColorRepresentationManager::ColorRepresentationManager(ColorRepresentationManager&&) noexcept =
    default;
ColorRepresentationManager& ColorRepresentationManager::operator=(
    ColorRepresentationManager&&) noexcept = default;

Result<ColorRepresentationManager> ColorRepresentationManager::create(
    Display& display, ColorRepresentationSupport support) {
    auto impl = std::make_unique<Impl>();
    impl->support = std::move(support);
    auto global = create_wl_global<&wp_color_representation_manager_v1_interface, cr_manager_bind>(
        display, 1, impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return ColorRepresentationManager{std::move(impl)};
}

} // namespace luminaria
