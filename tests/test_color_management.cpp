// wp-color-management-v1 and wp-color-representation-v1.
//
// Two compositors, one binary:
//
//   A. `ColorSupport::full()`. A client builds an HDR10 description (BT.2020,
//      PQ, 203-nit white, mastering display, MaxCLL/MaxFALL), puts it on a
//      surface and commits, together with a straight-alpha BT.709/limited
//      representation: the server must see exactly that on the Surface after
//      the commit. The client then reads an output's description back
//      (primaries, transfer function, luminances), and when the compositor says
//      the output turned HDR, and a surface's preferred description changed, it
//      is told — by `image_description_changed` and `preferred_changed2`.
//
//   B. The default, `ColorSupport::srgb_only()`. The same client sees only what
//      the compositor can honour — sRGB, no PQ, no custom primaries, no straight
//      alpha — an sRGB description goes through, and asking for PQ anyway is a
//      protocol error (`invalid_tf`), not a silently wrong picture.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "color-management-v1-client-protocol.h"
#include "color-representation-v1-client-protocol.h"

import luminaria;
import std;

namespace {

struct ClientState {
    wl_compositor* compositor = nullptr;
    wl_output* output = nullptr;
    wp_color_manager_v1* manager = nullptr;
    wp_color_representation_manager_v1* rep_manager = nullptr;

    // what the manager advertised
    std::set<std::uint32_t> features, tfs, primaries, intents;
    std::set<std::uint32_t> alpha_modes;
    std::set<std::pair<std::uint32_t, std::uint32_t>> coefficients;
    bool manager_done = false;

    // image descriptions
    std::uint64_t ready_identity = 0;
    int ready_count = 0;

    // information about the output's description
    std::uint32_t info_primaries_named = 0, info_tf_named = 0;
    std::uint32_t info_min = 0, info_max = 0, info_ref = 0;
    bool info_done = false;

    int output_changes = 0;
    int preferred_changes = 0;
    std::uint64_t preferred_identity = 0;
};
ClientState g_c;

// ---- manager
void m_intent(void*, wp_color_manager_v1*, uint32_t v) { g_c.intents.insert(v); }
void m_feature(void*, wp_color_manager_v1*, uint32_t v) { g_c.features.insert(v); }
void m_tf(void*, wp_color_manager_v1*, uint32_t v) { g_c.tfs.insert(v); }
void m_primaries(void*, wp_color_manager_v1*, uint32_t v) { g_c.primaries.insert(v); }
void m_done(void*, wp_color_manager_v1*) { g_c.manager_done = true; }
const wp_color_manager_v1_listener kManager{m_intent, m_feature, m_tf, m_primaries, m_done};

void rm_alpha(void*, wp_color_representation_manager_v1*, uint32_t v) { g_c.alpha_modes.insert(v); }
void rm_coeffs(void*, wp_color_representation_manager_v1*, uint32_t c, uint32_t r) {
    g_c.coefficients.insert({c, r});
}
void rm_done(void*, wp_color_representation_manager_v1*) {}
const wp_color_representation_manager_v1_listener kRepManager{rm_alpha, rm_coeffs, rm_done};

// ---- image descriptions
void d_failed(void*, wp_image_description_v1*, uint32_t, const char*) {}
void d_ready(void*, wp_image_description_v1*, uint32_t identity) {
    g_c.ready_identity = identity;
    ++g_c.ready_count;
}
void d_ready2(void*, wp_image_description_v1*, uint32_t hi, uint32_t lo) {
    g_c.ready_identity = (static_cast<std::uint64_t>(hi) << 32) | lo;
    ++g_c.ready_count;
}
const wp_image_description_v1_listener kDesc{d_failed, d_ready, d_ready2};

void i_done(void*, wp_image_description_info_v1*) { g_c.info_done = true; }
void i_icc(void*, wp_image_description_info_v1*, int32_t fd, uint32_t) { close(fd); }
void i_primaries(void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t,
                 int32_t, int32_t, int32_t) {}
void i_primaries_named(void*, wp_image_description_info_v1*, uint32_t p) {
    g_c.info_primaries_named = p;
}
void i_tf_power(void*, wp_image_description_info_v1*, uint32_t) {}
void i_tf_named(void*, wp_image_description_info_v1*, uint32_t tf) { g_c.info_tf_named = tf; }
void i_luminances(void*, wp_image_description_info_v1*, uint32_t min, uint32_t max, uint32_t ref) {
    g_c.info_min = min;
    g_c.info_max = max;
    g_c.info_ref = ref;
}
void i_target_primaries(void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t, int32_t) {}
void i_target_luminance(void*, wp_image_description_info_v1*, uint32_t, uint32_t) {}
void i_target_max_cll(void*, wp_image_description_info_v1*, uint32_t) {}
void i_target_max_fall(void*, wp_image_description_info_v1*, uint32_t) {}
const wp_image_description_info_v1_listener kInfo{
    i_done,       i_icc,           i_primaries,     i_primaries_named,  i_tf_power,
    i_tf_named,   i_luminances,    i_target_primaries, i_target_luminance, i_target_max_cll,
    i_target_max_fall};

void o_changed(void*, wp_color_management_output_v1*) { ++g_c.output_changes; }
const wp_color_management_output_v1_listener kOutput{o_changed};

void f_preferred(void*, wp_color_management_surface_feedback_v1*, uint32_t) {
    ++g_c.preferred_changes;
}
void f_preferred2(void*, wp_color_management_surface_feedback_v1*, uint32_t hi, uint32_t lo) {
    ++g_c.preferred_changes;
    g_c.preferred_identity = (static_cast<std::uint64_t>(hi) << 32) | lo;
}
const wp_color_management_surface_feedback_v1_listener kFeedback{f_preferred, f_preferred2};

void registry_global(void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
    if (!std::strcmp(iface, "wl_compositor")) {
        g_c.compositor = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (!std::strcmp(iface, "wl_output")) {
        g_c.output = static_cast<wl_output*>(
            wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4));
    } else if (!std::strcmp(iface, "wp_color_manager_v1")) {
        g_c.manager = static_cast<wp_color_manager_v1*>(
            wl_registry_bind(reg, name, &wp_color_manager_v1_interface, 3));
        wp_color_manager_v1_add_listener(g_c.manager, &kManager, nullptr);
    } else if (!std::strcmp(iface, "wp_color_representation_manager_v1")) {
        g_c.rep_manager = static_cast<wp_color_representation_manager_v1*>(
            wl_registry_bind(reg, name, &wp_color_representation_manager_v1_interface, 1));
        wp_color_representation_manager_v1_add_listener(g_c.rep_manager, &kRepManager, nullptr);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry{registry_global, registry_global_remove};

struct Outcome {
    bool finished = false;
    int protocol_error = 0; // scenario B: the code of the error we provoked
    const wl_interface* error_iface = nullptr;
    std::uint64_t surface_identity = 0;
    std::uint64_t first_identity = 0;
    std::uint64_t feedback_identity = 0;
} g_o;

enum class Scenario { full, srgb_only };
Scenario g_scenario = Scenario::full;

void roundtrips(wl_display* d, int n = 3) {
    for (int i = 0; i < n; ++i) {
        wl_display_roundtrip(d);
    }
}

constexpr uint32_t kPrimarySrgb = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
constexpr uint32_t kPrimaryBt2020 = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
constexpr uint32_t kTfPq = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
constexpr uint32_t kTfSrgb = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;

wp_image_description_v1* make_srgb(wp_color_manager_v1* mgr) {
    auto* creator = wp_color_manager_v1_create_parametric_creator(mgr);
    wp_image_description_creator_params_v1_set_tf_named(creator, kTfSrgb);
    wp_image_description_creator_params_v1_set_primaries_named(creator, kPrimarySrgb);
    auto* desc = wp_image_description_creator_params_v1_create(creator);
    wp_image_description_v1_add_listener(desc, &kDesc, nullptr);
    return desc;
}

void run_client(int fd) {
    wl_display* display = wl_display_connect_to_fd(fd);
    if (display == nullptr) {
        return;
    }
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistry, nullptr);
    wl_display_roundtrip(display);
    roundtrips(display, 2);
    assert(g_c.compositor && g_c.output && g_c.manager && g_c.rep_manager && g_c.manager_done);
    assert(g_c.features.contains(WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC));
    assert(!g_c.features.contains(WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4)); // never

    wl_surface* surface = wl_compositor_create_surface(g_c.compositor);
    wp_color_management_surface_v1* cm_surface =
        wp_color_manager_v1_get_surface(g_c.manager, surface);
    wp_color_representation_surface_v1* rep_surface =
        wp_color_representation_manager_v1_get_surface(g_c.rep_manager, surface);

    if (g_scenario == Scenario::srgb_only) {
        // Only what the compositor can honour is on offer.
        assert(g_c.primaries == std::set<std::uint32_t>{kPrimarySrgb});
        assert(g_c.tfs.contains(kTfSrgb) && !g_c.tfs.contains(kTfPq));
        assert(!g_c.features.contains(WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES));
        assert(!g_c.features.contains(WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER));
        assert(g_c.alpha_modes == std::set<std::uint32_t>{WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL});

        // An sRGB surface is fine.
        auto* desc = make_srgb(g_c.manager);
        roundtrips(display, 2);
        assert(g_c.ready_count == 1);
        wp_color_management_surface_v1_set_image_description(
            cm_surface, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        wl_surface_commit(surface);
        roundtrips(display, 2);
        g_o.surface_identity = g_c.ready_identity;

        // PQ is not: a protocol error, not a quietly wrong picture.
        auto* creator = wp_color_manager_v1_create_parametric_creator(g_c.manager);
        wp_image_description_creator_params_v1_set_tf_named(creator, kTfPq);
        wl_display_roundtrip(display);
        g_o.protocol_error = wl_display_get_protocol_error(display, &g_o.error_iface, nullptr);
        g_o.finished = true;
        wl_display_disconnect(display);
        return;
    }

    // ---- scenario A
    assert(g_c.primaries.contains(kPrimaryBt2020) && g_c.tfs.contains(kTfPq));
    assert(g_c.features.contains(WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES));
    assert(!g_c.tfs.contains(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB)); // v3: deprecated
    assert(g_c.alpha_modes.contains(WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT));

    // The first thing a client does: ask what the output is.
    wp_color_management_output_v1* cm_output =
        wp_color_manager_v1_get_output(g_c.manager, g_c.output);
    wp_color_management_output_v1_add_listener(cm_output, &kOutput, nullptr);
    auto* out_desc = wp_color_management_output_v1_get_image_description(cm_output);
    wp_image_description_v1_add_listener(out_desc, &kDesc, nullptr);
    roundtrips(display, 2);
    assert(g_c.ready_count == 1);
    g_o.first_identity = g_c.ready_identity;
    auto* info = wp_image_description_v1_get_information(out_desc);
    wp_image_description_info_v1_add_listener(info, &kInfo, nullptr);
    roundtrips(display, 2);
    assert(g_c.info_done && g_c.info_primaries_named == kPrimarySrgb && g_c.info_tf_named == kTfSrgb);
    assert(g_c.info_min == 2000 && g_c.info_max == 80 && g_c.info_ref == 80); // 0.2 nits, in 1/10000

    // The preferred description for the surface.
    auto* feedback = wp_color_manager_v1_get_surface_feedback(g_c.manager, surface);
    wp_color_management_surface_feedback_v1_add_listener(feedback, &kFeedback, nullptr);
    roundtrips(display, 1);

    // HDR10 for the surface.
    auto* creator = wp_color_manager_v1_create_parametric_creator(g_c.manager);
    wp_image_description_creator_params_v1_set_tf_named(creator, kTfPq);
    wp_image_description_creator_params_v1_set_primaries_named(creator, kPrimaryBt2020);
    wp_image_description_creator_params_v1_set_luminances(creator, 50, 10000, 203);
    wp_image_description_creator_params_v1_set_mastering_display_primaries(
        creator, 680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000);
    wp_image_description_creator_params_v1_set_mastering_luminance(creator, 50, 1000);
    wp_image_description_creator_params_v1_set_max_cll(creator, 1000);
    wp_image_description_creator_params_v1_set_max_fall(creator, 400);
    auto* hdr = wp_image_description_creator_params_v1_create(creator);
    wp_image_description_v1_add_listener(hdr, &kDesc, nullptr);
    roundtrips(display, 2);
    assert(g_c.ready_count == 2);
    g_o.surface_identity = g_c.ready_identity;
    // A client-made description cannot be read back.
    wp_color_management_surface_v1_set_image_description(
        cm_surface, hdr, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_color_representation_surface_v1_set_alpha_mode(
        rep_surface, WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT);
    wp_color_representation_surface_v1_set_coefficients_and_range(
        rep_surface, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
        WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);
    wp_color_representation_surface_v1_set_chroma_location(
        rep_surface, WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_2);
    wl_surface_commit(surface);
    roundtrips(display, 2);

    // The same description built twice is the same description.
    auto* again = wp_color_manager_v1_create_parametric_creator(g_c.manager);
    wp_image_description_creator_params_v1_set_tf_named(again, kTfPq);
    wp_image_description_creator_params_v1_set_primaries_named(again, kPrimaryBt2020);
    wp_image_description_creator_params_v1_set_luminances(again, 50, 10000, 203);
    wp_image_description_creator_params_v1_set_mastering_display_primaries(
        again, 680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000);
    wp_image_description_creator_params_v1_set_mastering_luminance(again, 50, 1000);
    wp_image_description_creator_params_v1_set_max_cll(again, 1000);
    wp_image_description_creator_params_v1_set_max_fall(again, 400);
    auto* hdr2 = wp_image_description_creator_params_v1_create(again);
    wp_image_description_v1_add_listener(hdr2, &kDesc, nullptr);
    roundtrips(display, 2);
    assert(g_c.ready_identity == g_o.surface_identity);

    // The server now changes its mind about the output and the surface.
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        wl_display_roundtrip(display);
    }
    auto* preferred = wp_color_management_surface_feedback_v1_get_preferred(feedback);
    wp_image_description_v1_add_listener(preferred, &kDesc, nullptr);
    roundtrips(display, 2);
    g_o.feedback_identity = g_c.ready_identity;

    g_o.finished = true;
    wl_display_disconnect(display);
}

struct DestroyCtx {
    wl_listener listener;
    luminaria::Display* display;
};
void on_client_destroy(wl_listener* l, void*) {
    reinterpret_cast<DestroyCtx*>(reinterpret_cast<char*>(l) - offsetof(DestroyCtx, listener))
        ->display->terminate();
}

void run(Scenario scenario) {
    g_c = ClientState{};
    g_o = Outcome{};
    g_scenario = scenario;

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    auto display = luminaria::Display::create();
    assert(display.has_value());
    auto compositor = luminaria::Compositor::create(*display);
    assert(compositor.has_value());
    auto output = luminaria::OutputGlobal::create(*display, 800, 600);
    assert(output.has_value());

    const bool full = scenario == Scenario::full;
    auto colors = luminaria::ColorManager::create(
        *display, full ? luminaria::ColorSupport::full() : luminaria::ColorSupport::srgb_only());
    assert(colors.has_value());
    luminaria::ColorRepresentationSupport rep_support;
    if (full) {
        rep_support.alpha_modes.push_back(luminaria::AlphaMode::straight);
        rep_support.coefficients.push_back({luminaria::Coefficients::bt709, luminaria::ColorRange::limited});
    }
    auto reps = luminaria::ColorRepresentationManager::create(*display, rep_support);
    assert(reps.has_value());

    // What the Surface says after each commit.
    struct Seen {
        bool described = false, srgb = false, hdr = false;
        luminaria::RenderIntent intent = luminaria::RenderIntent::absolute;
        luminaria::ImageDescription description;
        luminaria::ColorRepresentation representation;
        int commits = 0;
    } seen;
    luminaria::Surface* the_surface = nullptr;
    luminaria::Signal<luminaria::SurfaceCommit>::Connection on_commit;
    auto ns = compositor->new_surface().connect([&](luminaria::NewSurface& e) {
        the_surface = &e.surface;
        on_commit = e.surface.commit.connect([&](luminaria::SurfaceCommit& c) {
            ++seen.commits;
            if (const auto* d = c.surface.image_description(); d != nullptr) {
                seen.described = true;
                seen.description = *d;
                seen.srgb = d->is_srgb();
                seen.hdr = d->is_hdr();
            }
            seen.intent = c.surface.render_intent();
            seen.representation = c.surface.color_representation();
        });
    });

    auto change = display->event_loop().add_timer([&] {
        colors->set_output_description(*output, luminaria::ImageDescription::bt2100_pq());
        if (the_surface != nullptr) {
            colors->set_preferred(*the_surface, luminaria::ImageDescription::bt2100_pq());
        }
    });
    if (full) {
        change.arm(150);
    }

    wl_client* client = wl_client_create(display->c_ptr(), fds[0]);
    assert(client != nullptr);
    DestroyCtx destroy_ctx{{}, &*display};
    destroy_ctx.listener.notify = on_client_destroy;
    wl_client_add_destroy_listener(client, &destroy_ctx.listener);
    std::thread client_thread(run_client, fds[1]);
    auto timeout = display->event_loop().add_timer([&] { display->terminate(); });
    timeout.arm(5000);
    display->run();
    client_thread.join();

    assert(g_o.finished);
    if (!full) {
        assert(seen.described && seen.srgb && !seen.hdr);
        assert(g_o.protocol_error == WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF);
        assert(g_o.error_iface == &wp_image_description_creator_params_v1_interface);
        return;
    }
    // The surface carries what the client declared.
    assert(seen.described && seen.hdr && !seen.srgb);
    const luminaria::ImageDescription& d = seen.description;
    assert(d.transfer_function == luminaria::TransferFunction::st2084_pq);
    assert(d.named_primaries == luminaria::Primaries::bt2020);
    assert(d.min_luminance == 0.005 && d.reference_luminance == 203.0);
    assert(d.max_luminance == d.min_luminance + 10000.0); // PQ pins the top of its range
    assert(d.max_cll == 1000u && d.max_fall == 400u);
    assert(d.target_luminance.has_value() && d.target_luminance->second == 1000.0);
    assert(d.target_primaries.has_value() && d.target_primaries->red.x == 0.68);
    assert(d.identity == g_o.surface_identity && d.identity != 0);
    assert(seen.intent == luminaria::RenderIntent::perceptual);
    // ...and what it said about its pixels.
    assert(seen.representation.alpha_mode == luminaria::AlphaMode::straight);
    assert(seen.representation.coefficients == luminaria::Coefficients::bt709);
    assert(seen.representation.range == luminaria::ColorRange::limited);
    assert(seen.representation.chroma_location == luminaria::ChromaLocation::type_2);
    // It was told the output turned HDR, and what the surface prefers now.
    assert(g_c.output_changes == 1);
    assert(g_c.preferred_changes == 1);
    assert(g_o.feedback_identity == g_c.preferred_identity);
    assert(g_o.feedback_identity != g_o.first_identity);
}

} // namespace

int main() {
    run(Scenario::full);
    run(Scenario::srgb_only);
    return 0;
}
