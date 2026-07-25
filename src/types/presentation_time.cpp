module;

// Implements wp_presentation (version 2). Feedback objects queue up per
// surface; the compositor drains a surface's queue when the frame carrying it
// is reported presented, and both `presented` and `discarded` are destructors,
// so a queue entry is used exactly once.

#include <ctime>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <wayland-server-core.h>

#include "presentation-time-protocol.h"

module luminaria;

namespace luminaria {

struct Presentation::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    // Feedback objects still waiting, per surface, oldest first. The connection
    // clears the entry if the surface dies before its frame is presented.
    struct Pending {
        std::vector<wl_resource*> feedbacks;
        Signal<SurfaceDestroy>::Connection on_destroy;
    };
    std::map<Surface*, Pending> pending;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }

    void add(Surface& surface, wl_resource* feedback) {
        Pending& p = pending[&surface];
        if (!p.on_destroy.connected()) {
            Surface* key = &surface;
            p.on_destroy = surface.destroy.connect([this, key](SurfaceDestroy& e) {
                // Move the list out first: wl_resource_destroy re-enters through
                // on_feedback_destroy, which edits the very vector we'd be walking.
                const std::vector<wl_resource*> orphans = take(e.surface);
                pending.erase(key);
                for (wl_resource* r : orphans) {
                    wp_presentation_feedback_send_discarded(r);
                    wl_resource_destroy(r);
                }
            });
        }
        p.feedbacks.push_back(feedback);
    }

    std::vector<wl_resource*> take(Surface& surface) {
        auto it = pending.find(&surface);
        if (it == pending.end()) {
            return {};
        }
        std::vector<wl_resource*> out = std::move(it->second.feedbacks);
        it->second.feedbacks.clear();
        return out;
    }

    void forget(wl_resource* feedback) {
        for (auto& [surface, p] : pending) {
            std::erase(p.feedbacks, feedback);
        }
    }
};

namespace {

void presentation_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void on_feedback_destroy(wl_resource* resource) {
    // A client may drop a feedback object before we present. Nothing to send;
    // just stop tracking it.
    auto* impl = static_cast<Presentation::Impl*>(wl_resource_get_user_data(resource));
    if (impl != nullptr) {
        impl->forget(resource);
    }
}

void presentation_feedback(wl_client* client, wl_resource* resource, wl_resource* surface_resource,
                           uint32_t callback) {
    auto* impl = static_cast<Presentation::Impl*>(wl_resource_get_user_data(resource));
    wl_resource* feedback =
        wl_resource_create(client, &wp_presentation_feedback_interface,
                           wl_resource_get_version(resource), static_cast<int>(callback));
    if (feedback == nullptr) {
        wl_resource_post_no_memory(resource);
        return;
    }
    // wp_presentation_feedback has no requests, so a null vtable can never be
    // dereferenced — it only carries user data and the destroy hook.
    wl_resource_set_implementation(feedback, nullptr, impl, on_feedback_destroy);

    Surface* surface = surface_from_resource(surface_resource);
    if (surface == nullptr) {
        // Nothing will ever present it; say so immediately rather than leaving
        // the client's animation clock stalled.
        wp_presentation_feedback_send_discarded(feedback);
        wl_resource_destroy(feedback);
        return;
    }
    impl->add(*surface, feedback);
}

constexpr struct wp_presentation_interface presentation_impl = {
    .destroy = presentation_destroy_request,
    .feedback = presentation_feedback,
};

void presentation_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &wp_presentation_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &presentation_impl, data, nullptr);
    // Our timestamps come from Output::present, which reports CLOCK_MONOTONIC.
    wp_presentation_send_clock_id(resource, CLOCK_MONOTONIC);
}

} // namespace

Presentation::Presentation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Presentation::~Presentation() = default;
Presentation::Presentation(Presentation&&) noexcept = default;
Presentation& Presentation::operator=(Presentation&&) noexcept = default;

Result<Presentation> Presentation::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->global =
        wl_global_create(impl->display, &wp_presentation_interface, 2, impl.get(), presentation_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(wp_presentation) failed");
    }
    return Presentation{std::move(impl)};
}

void Presentation::notify_presented(Surface& surface, const PresentEvent& event,
                                    wl_resource* sync_output) {
    uint32_t flags = 0;
    if (event.vsync) {
        flags |= WP_PRESENTATION_FEEDBACK_KIND_VSYNC;
    }
    if (event.hw_clock) {
        flags |= WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK;
    }
    if (event.seq != 0) {
        flags |= WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION;
    }
    for (wl_resource* feedback : impl_->take(surface)) {
        if (sync_output != nullptr) {
            wp_presentation_feedback_send_sync_output(feedback, sync_output);
        }
        wp_presentation_feedback_send_presented(
            feedback, static_cast<uint32_t>(event.tv_sec >> 32),
            static_cast<uint32_t>(event.tv_sec & 0xFFFFFFFFu), event.tv_nsec, event.refresh_ns,
            static_cast<uint32_t>(event.seq >> 32), static_cast<uint32_t>(event.seq & 0xFFFFFFFFu),
            flags);
        wl_resource_destroy(feedback);
    }
}

void Presentation::notify_discarded(Surface& surface) {
    for (wl_resource* feedback : impl_->take(surface)) {
        wp_presentation_feedback_send_discarded(feedback);
        wl_resource_destroy(feedback);
    }
}

} // namespace luminaria
