module;

// Implements xdg_activation_v1 (version 1).
//
// A committed xdg_activation_token_v1 becomes a random string plus the context
// it was issued in. That context is what the compositor judges the later
// `activate` on, so it is kept here until the token is used exactly once.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <sys/random.h>
#include <sys/types.h>

#include <wayland-server-core.h>

#include "xdg-activation-v1-protocol.h"

module luminaria;

namespace luminaria {

struct XdgActivation::Impl {
    wl_display* display = nullptr;
    wl_global* global = nullptr;
    Signal<ActivationTokenRequest> new_token;
    Signal<ActivationRequest> request_activate;

    /// A token that has been handed out and not yet used.
    struct Issued {
        std::string token;
        std::string app_id;
        Surface* surface = nullptr;
        wl_resource* seat = nullptr;
        std::uint32_t serial = 0;
        bool granted = true;
        // The requesting surface can die before the token is redeemed.
        Signal<SurfaceDestroy>::Connection surface_gone;
    };
    // Tokens a client asks for and never uses are never redeemed, so the list is
    // capped and the oldest entry falls off. 32 outstanding activations is far
    // more than any real desktop has in flight.
    static constexpr std::size_t kMaxIssued = 32;
    // Held by pointer: each entry's surface-destroy subscription captures its
    // own address, and the list is erased from the middle when a token is used.
    std::vector<std::unique_ptr<Issued>> issued;

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }

    /// 128 random bits in hex. Unguessable is the whole security property — a
    /// client that can guess a token can raise its own window whenever it likes
    /// — so this comes from the kernel CSPRNG, not from <random>, whose
    /// generators are predictable once you have seen enough output.
    /// Empty if the kernel would not give us entropy, in which case no token is
    /// recorded at all — better to hand the client one that can never be
    /// redeemed than one an attacker could guess.
    static std::string mint() {
        std::array<unsigned char, 16> bytes{};
        std::size_t got = 0;
        while (got < bytes.size()) {
            const ssize_t n = getrandom(bytes.data() + got, bytes.size() - got, 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                return {};
            }
            got += static_cast<std::size_t>(n);
        }
        constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char byte : bytes) {
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0xF]);
        }
        return out;
    }
};

namespace {

using Impl = XdgActivation::Impl;

/// One xdg_activation_token_v1 being built up. Owned by its resource.
struct TokenBuilder {
    Impl* impl = nullptr;
    wl_resource* seat = nullptr;
    std::uint32_t serial = 0;
    Surface* surface = nullptr;
    std::string app_id;
    bool committed = false;
    Signal<SurfaceDestroy>::Connection surface_gone;
};

TokenBuilder* builder_of(wl_resource* resource) {
    return static_cast<TokenBuilder*>(wl_resource_get_user_data(resource));
}

void token_set_serial(wl_client*, wl_resource* resource, uint32_t serial, wl_resource* seat) {
    TokenBuilder* b = builder_of(resource);
    b->serial = serial;
    b->seat = seat;
}

void token_set_app_id(wl_client*, wl_resource* resource, const char* app_id) {
    builder_of(resource)->app_id = app_id != nullptr ? app_id : "";
}

void token_set_surface(wl_client*, wl_resource* resource, wl_resource* surface_resource) {
    TokenBuilder* b = builder_of(resource);
    b->surface = surface_from_resource(surface_resource);
    b->surface_gone = {};
    if (b->surface != nullptr) {
        b->surface_gone =
            b->surface->destroy.connect([b](SurfaceDestroy&) { b->surface = nullptr; });
    }
}

void token_commit(wl_client* client, wl_resource* resource) {
    TokenBuilder* b = builder_of(resource);
    if (b->committed) {
        wl_resource_post_error(resource, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
                               "the activation token has already been committed");
        return;
    }
    b->committed = true;

    ActivationTokenRequest event{client, b->seat, b->serial, b->surface, b->app_id, true};
    b->impl->new_token.emit(event);

    auto issued = std::make_unique<Impl::Issued>();
    issued->token = Impl::mint();
    if (issued->token.empty()) {
        // Nothing is recorded, so this token matches nothing later.
        xdg_activation_token_v1_send_done(resource, "");
        return;
    }
    issued->app_id = b->app_id;
    issued->surface = b->surface;
    issued->seat = b->seat;
    issued->serial = b->serial;
    issued->granted = event.granted;
    if (issued->surface != nullptr) {
        Impl::Issued* slot = issued.get();
        issued->surface_gone =
            issued->surface->destroy.connect([slot](SurfaceDestroy&) { slot->surface = nullptr; });
    }
    if (b->impl->issued.size() >= Impl::kMaxIssued) {
        b->impl->issued.erase(b->impl->issued.begin());
    }
    b->impl->issued.push_back(std::move(issued));

    xdg_activation_token_v1_send_done(resource, b->impl->issued.back()->token.c_str());
}

void token_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct xdg_activation_token_v1_interface token_impl = {
    .set_serial = token_set_serial,
    .set_app_id = token_set_app_id,
    .set_surface = token_set_surface,
    .commit = token_commit,
    .destroy = token_destroy_request,
};

void token_resource_destroy(wl_resource* resource) {
    delete builder_of(resource);
}

void activation_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void activation_get_token(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* impl = static_cast<Impl*>(wl_resource_get_user_data(resource));
    wl_resource* token = wl_resource_create(client, &xdg_activation_token_v1_interface,
                                            wl_resource_get_version(resource), id);
    if (token == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(token, &token_impl, new TokenBuilder{.impl = impl},
                                   token_resource_destroy);
}

void activation_activate(wl_client*, wl_resource* resource, const char* token,
                         wl_resource* surface_resource) {
    auto* impl = static_cast<Impl*>(wl_resource_get_user_data(resource));
    Surface* surface = surface_from_resource(surface_resource);
    if (surface == nullptr) {
        return;
    }
    const std::string wanted = token != nullptr ? token : "";
    auto it = std::find_if(
        impl->issued.begin(), impl->issued.end(),
        [&wanted](const std::unique_ptr<Impl::Issued>& i) { return i->token == wanted; });

    ActivationRequest event{*surface, false, {}, nullptr, nullptr, 0};
    if (it != impl->issued.end()) {
        event.token_valid = (*it)->granted;
        event.app_id = (*it)->app_id;
        event.requesting_surface = (*it)->surface;
        event.seat = (*it)->seat;
        event.serial = (*it)->serial;
        // Single use, whether or not the compositor honours it: a token that
        // could be replayed is a token that can raise a window at any later time.
        impl->issued.erase(it);
    }
    impl->request_activate.emit(event);
}

constexpr struct xdg_activation_v1_interface activation_impl = {
    .destroy = activation_destroy_request,
    .get_activation_token = activation_get_token,
    .activate = activation_activate,
};

void activation_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &xdg_activation_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &activation_impl, data, nullptr);
}

} // namespace

XdgActivation::XdgActivation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
XdgActivation::~XdgActivation() = default;
XdgActivation::XdgActivation(XdgActivation&&) noexcept = default;
XdgActivation& XdgActivation::operator=(XdgActivation&&) noexcept = default;

Result<XdgActivation> XdgActivation::create(Display& display) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->global = wl_global_create(impl->display, &xdg_activation_v1_interface, 1, impl.get(),
                                    activation_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(xdg_activation_v1) failed");
    }
    return XdgActivation{std::move(impl)};
}

Signal<ActivationTokenRequest>& XdgActivation::new_token() noexcept { return impl_->new_token; }

Signal<ActivationRequest>& XdgActivation::request_activate() noexcept {
    return impl_->request_activate;
}

} // namespace luminaria
