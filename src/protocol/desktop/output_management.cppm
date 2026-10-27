// luminaria/output_management.cppm — the zwlr_output_manager_v1 global.
//
// wlr-output-management is how wlr-randr, kanshi and display-settings panels
// see every monitor (a "head") and ask for a different arrangement: mode,
// position, transform, scale, on or off. Asking is all they do. The heads are
// described from what is already true — `Output` for the modes, scale and
// transform, `OutputLayout` for where it sits and whether it is on at all — and
// a client's apply or test arrives as one `OutputConfigRequest` holding the
// whole configuration, answered with the verdict the handler leaves in it.
//
// Not in the core module (ADR 0003): it hands one client every screen, and a
// nested or kiosk compositor has no business advertising that by default.
//
// Importing luminaria pulls in no libwayland headers: the C types this
// interface names are forward-declared in the global module fragment.

module;

#include <cstdint>
#include <typeinfo>

#include <wayland-server-core.h>
#include "wlr-output-management-unstable-v1-protocol.h"

export module luminaria.desktop:output_management;

import std;

import luminaria;

export namespace luminaria {

/// What a head is called and what it is. Fixed for the head's life: the
/// protocol has no event that changes any of it.
struct OutputHeadInfo {
    std::string name;        ///< what wlr-randr/kanshi address it by; match the wl_output name
    std::string description; ///< human-readable; empty means `name`
    std::string make;        ///< these three are sent only when non-empty
    std::string model;
    std::string serial_number;
    int physical_width_mm = 0; ///< 0 for a head with no physical size (virtual, projector)
    int physical_height_mm = 0;
};

struct OutputPosition {
    int x = 0;
    int y = 0;
};

/// One head as a client wants it. An unset field means "leave it as it is".
struct OutputHeadConfig {
    Output* output = nullptr; ///< valid for the duration of the request
    bool enabled = true;
    /// One of the modes the head advertised, or — with `custom_mode` — a size
    /// and refresh the client made up, which nothing has checked yet.
    std::optional<OutputMode> mode;
    bool custom_mode = false;
    std::optional<OutputPosition> position; ///< layout coordinates
    std::optional<Transform> transform;
    std::optional<double> scale; ///< as sent; may be fractional and `Output::scale` is not
    std::optional<bool> adaptive_sync; ///< variable refresh; apply with `Output::set_vrr()`
};

/// What the client is told. Anything but `succeeded` means nothing changed:
/// a handler that fails half-way through an apply rolls back first.
enum class OutputConfigVerdict : std::uint8_t {
    succeeded,
    failed,
    cancelled, ///< the configuration was built against outputs that have since changed
};

/// A client applied or tested a configuration. Covers every head the manager
/// advertises, exactly once each, in the order they were added. The handler
/// answers by setting `verdict`; nobody answering is a refusal.
///
/// A configuration built against a stale picture of the outputs (a hotplug or
/// an `update()` landed in between) is cancelled without being emitted at all.
struct OutputConfigRequest {
    bool test = false; ///< only say whether it would work; change nothing
    std::vector<OutputHeadConfig> heads;
    OutputConfigVerdict verdict = OutputConfigVerdict::failed;
};

/// The zwlr_output_manager_v1 global (version 4). Move-only; pointer-stable
/// state so the libwayland global can hold a pointer to it.
///
/// A head is enabled exactly when its output is in the layout. `layout` is not
/// owned and must outlive the manager.
class OutputManager {
public:
    [[nodiscard]] static Result<OutputManager> create(Display& display,
                                                      const OutputLayout& layout);

    ~OutputManager();
    OutputManager(OutputManager&&) noexcept;
    OutputManager& operator=(OutputManager&&) noexcept;
    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    /// Advertise `output` as a head, with its state as it is now. It leaves on
    /// its own when the output is destroyed. Adding it twice does nothing.
    void add_head(Output& output, OutputHeadInfo info);
    /// Stop advertising `output` (zwlr_output_head_v1.finished).
    void remove_head(const Output& output);

    /// Re-read every head and send clients what changed since last time, as
    /// one atomic update with a new serial. Sends nothing if nothing changed.
    /// Nothing here notices a change by itself — call it after a mode switch,
    /// a scale or transform change, or a move in the layout.
    void update();

    /// Fires on each apply and test. See `OutputConfigRequest`.
    [[nodiscard]] Signal<OutputConfigRequest>& request() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit OutputManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements zwlr_output_manager_v1 (version 4).
//
// Every bound manager gets its own head and mode objects for the same server
// heads, so the state lives once in Impl (as last sent) and each binding only
// holds the resources that address it. A resource the client still holds
// outlives what it described — a head that was unplugged, a mode list that was
// replaced, the manager itself — so each one's user data has a back-pointer
// that is nulled, and the object turns inert, rather than dangling.

namespace luminaria {

// The Om* types are lifted out of the anonymous namespace below: Impl's member
// signatures name them, and a module-linkage declaration may not expose a
// TU-local type. Still unexported — private to the module.

/// A head as clients were last told about it.
struct OmHeadState {
    bool enabled = false;
    std::vector<OutputMode> modes;
    std::size_t current = 0; // index into `modes`
    int x = 0;
    int y = 0;
    Transform transform = Transform::normal;
    int scale = 1;
    bool adaptive_sync = false;
};

struct OmHead {
    std::uint32_t id = 0; // never reused, so a stale id resolves to nothing
    Output* output = nullptr;
    OutputHeadInfo info;
    OmHeadState sent;
    Signal<OutputDestroy>::Connection on_destroy;
};

struct OmBinding;
struct OmHeadRes;

/// user_data of a zwlr_output_mode_v1. Inert (no owner) once its head is gone
/// or the mode list it belonged to was replaced.
struct OmModeRes {
    OmHeadRes* owner = nullptr;
    wl_resource* resource = nullptr;
    OutputMode mode;
};

/// user_data of a zwlr_output_head_v1. `head` is 0 once the head finished;
/// `binding` is null once the manager resource (or the manager) is gone.
struct OmHeadRes {
    OmBinding* binding = nullptr;
    wl_resource* resource = nullptr;
    std::uint32_t head = 0;
    std::vector<OmModeRes*> modes; // parallel to OmHead::sent.modes; null once released
};

/// One bound zwlr_output_manager_v1.
struct OmBinding {
    wl_resource* resource = nullptr;
    OutputManager::Impl* impl = nullptr;
    std::vector<OmHeadRes*> heads;
};

struct OmConfig;

/// One enable_head or disable_head. Owned by its configuration; the
/// zwlr_output_configuration_head_v1 (enable only) points at it.
struct OmConfigHead {
    OmConfig* config = nullptr;
    wl_resource* resource = nullptr;
    std::uint32_t head = 0; // 0: the client named a head that had already finished
    OutputHeadConfig wanted;
};

/// user_data of a zwlr_output_configuration_v1.
struct OmConfig {
    OutputManager::Impl* impl = nullptr; // null once the manager is gone
    wl_resource* resource = nullptr;
    std::uint32_t serial = 0;
    bool used = false;  // applied or tested: only destroy is legal now
    bool stale = false; // named a head or mode that has since gone
    std::vector<std::unique_ptr<OmConfigHead>> heads;
};

struct OutputManager::Impl {
    WlGlobal global;
    const OutputLayout* layout = nullptr;
    Signal<OutputConfigRequest> request;
    std::vector<std::unique_ptr<OmHead>> heads;
    std::vector<std::unique_ptr<OmBinding>> bindings;
    std::vector<OmConfig*> configs; // outstanding, so they can be told the manager died
    std::uint32_t next_id = 1;
    std::uint32_t serial = 0;

    ~Impl();

    [[nodiscard]] OmHead* head(std::uint32_t id) const;
    [[nodiscard]] OmHeadState read(const Output& output) const;
    void send_head(OmBinding& b, const OmHead& h);
    void send_modes(OmHeadRes& r, const std::vector<OutputMode>& modes);
    void send_state(OmHeadRes& r, const OmHeadState& s, unsigned what);
    void finish_modes(OmHeadRes& r);
    void finish_head(OmHeadRes& r);
    void remove(const Output& output);
    void send_done();
};

namespace {

using OmImpl = OutputManager::Impl;

// Which parts of a head's state send_state() sends.
constexpr unsigned kOmEnabled = 1u << 0;
constexpr unsigned kOmCurrent = 1u << 1;
constexpr unsigned kOmPosition = 1u << 2;
constexpr unsigned kOmTransform = 1u << 3;
constexpr unsigned kOmScale = 1u << 4;
constexpr unsigned kOmAdaptive = 1u << 5;
constexpr unsigned kOmAll =
    kOmEnabled | kOmCurrent | kOmPosition | kOmTransform | kOmScale | kOmAdaptive;

// --- zwlr_output_mode_v1 / zwlr_output_head_v1 --------------------------------

void om_mode_resource_destroy(wl_resource* resource) {
    auto* m = static_cast<OmModeRes*>(wl_resource_get_user_data(resource));
    if (m->owner != nullptr) {
        std::ranges::replace(m->owner->modes, m, static_cast<OmModeRes*>(nullptr));
    }
    delete m;
}

constexpr struct zwlr_output_mode_v1_interface om_mode_impl = {
    .release = resource_destroy_request,
};

void om_head_resource_destroy(wl_resource* resource) {
    auto* r = static_cast<OmHeadRes*>(wl_resource_get_user_data(resource));
    for (OmModeRes* m : r->modes) {
        if (m != nullptr) {
            m->owner = nullptr;
        }
    }
    if (r->binding != nullptr) {
        std::erase(r->binding->heads, r);
    }
    delete r;
}

constexpr struct zwlr_output_head_v1_interface om_head_impl = {
    .release = resource_destroy_request,
};

// --- zwlr_output_configuration_head_v1 ----------------------------------------

/// The entry behind a configuration head, or null if nothing may be set on it
/// any more: its configuration is gone, or was already applied or tested.
OmConfigHead* om_settable(wl_resource* resource) {
    auto* e = static_cast<OmConfigHead*>(wl_resource_get_user_data(resource));
    return e != nullptr && !e->config->used ? e : nullptr;
}

bool om_already_set(wl_resource* resource, bool set, const char* what) {
    if (set) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_ALREADY_SET,
                               "%s has already been set", what);
    }
    return set;
}

void om_set_mode(wl_client*, wl_resource* resource, wl_resource* mode) {
    OmConfigHead* e = om_settable(resource);
    if (e == nullptr || om_already_set(resource, e->wanted.mode.has_value(), "mode")) {
        return;
    }
    const auto* m = static_cast<OmModeRes*>(wl_resource_get_user_data(mode));
    if (m->owner == nullptr) {
        // The mode went away after the client read it (a hotplug, a new mode
        // list). Not the client's fault; the configuration is just outdated.
        e->config->stale = true;
    } else if (m->owner->head != e->head) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_MODE,
                               "mode does not belong to this head");
        return;
    }
    e->wanted.mode = m->mode;
    e->wanted.custom_mode = false;
}

void om_set_custom_mode(wl_client*, wl_resource* resource, int32_t width, int32_t height,
                        int32_t refresh) {
    OmConfigHead* e = om_settable(resource);
    if (e == nullptr || om_already_set(resource, e->wanted.mode.has_value(), "mode")) {
        return;
    }
    if (width <= 0 || height <= 0 || refresh < 0) {
        wl_resource_post_error(resource,
                               ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_CUSTOM_MODE,
                               "invalid custom mode %dx%d@%d", width, height, refresh);
        return;
    }
    e->wanted.mode = OutputMode{width, height, refresh, false};
    e->wanted.custom_mode = true;
}

void om_set_position(wl_client*, wl_resource* resource, int32_t x, int32_t y) {
    OmConfigHead* e = om_settable(resource);
    if (e == nullptr || om_already_set(resource, e->wanted.position.has_value(), "position")) {
        return;
    }
    e->wanted.position = OutputPosition{x, y};
}

void om_set_transform(wl_client*, wl_resource* resource, int32_t transform) {
    OmConfigHead* e = om_settable(resource);
    if (e == nullptr || om_already_set(resource, e->wanted.transform.has_value(), "transform")) {
        return;
    }
    if (transform < 0 || transform > static_cast<int32_t>(Transform::flipped_270)) {
        wl_resource_post_error(resource,
                               ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_TRANSFORM,
                               "invalid transform %d", transform);
        return;
    }
    e->wanted.transform = static_cast<Transform>(transform);
}

void om_set_scale(wl_client*, wl_resource* resource, wl_fixed_t scale) {
    OmConfigHead* e = om_settable(resource);
    if (e == nullptr || om_already_set(resource, e->wanted.scale.has_value(), "scale")) {
        return;
    }
    const double value = wl_fixed_to_double(scale);
    if (value <= 0) {
        wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_SCALE,
                               "invalid scale %f", value);
        return;
    }
    e->wanted.scale = value;
}

void om_set_adaptive_sync(wl_client*, wl_resource* resource, uint32_t state) {
    OmConfigHead* e = om_settable(resource);
    if (e == nullptr ||
        om_already_set(resource, e->wanted.adaptive_sync.has_value(), "adaptive sync")) {
        return;
    }
    if (state > ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED) {
        wl_resource_post_error(
            resource, ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_ADAPTIVE_SYNC_STATE,
            "invalid adaptive sync state %u", state);
        return;
    }
    e->wanted.adaptive_sync = state == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED;
}

constexpr struct zwlr_output_configuration_head_v1_interface om_config_head_impl = {
    .set_mode = om_set_mode,
    .set_custom_mode = om_set_custom_mode,
    .set_position = om_set_position,
    .set_transform = om_set_transform,
    .set_scale = om_set_scale,
    .set_adaptive_sync = om_set_adaptive_sync,
};

void om_config_head_resource_destroy(wl_resource* resource) {
    // The entry belongs to the configuration and stays in it: the head is
    // still enabled with whatever was set on it.
    if (auto* e = static_cast<OmConfigHead*>(wl_resource_get_user_data(resource)); e != nullptr) {
        e->resource = nullptr;
    }
}

// --- zwlr_output_configuration_v1 ---------------------------------------------

OmConfig* om_config_of(wl_resource* resource) {
    return static_cast<OmConfig*>(wl_resource_get_user_data(resource));
}

/// False (with a protocol error posted) once the configuration was applied or
/// tested — after that, only destroy is legal.
bool om_unused(OmConfig& c) {
    if (c.used) {
        wl_resource_post_error(c.resource, ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
                               "configuration has already been applied or tested");
        return false;
    }
    return true;
}

/// A new entry for `head` (a zwlr_output_head_v1), or null after a protocol
/// error because that head is already in the configuration.
std::unique_ptr<OmConfigHead> om_new_entry(OmConfig& c, wl_resource* head, bool enabled) {
    const auto* r = static_cast<OmHeadRes*>(wl_resource_get_user_data(head));
    auto e = std::make_unique<OmConfigHead>();
    e->config = &c;
    e->head = r->head;
    e->wanted.enabled = enabled;
    if (e->head != 0) {
        for (const auto& other : c.heads) {
            if (other->head == e->head) {
                wl_resource_post_error(c.resource,
                                       ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD,
                                       "head has already been configured");
                return nullptr;
            }
        }
    }
    const OmHead* h = c.impl != nullptr && e->head != 0 ? c.impl->head(e->head) : nullptr;
    if (h == nullptr) {
        c.stale = true; // unplugged since the client saw it
    } else {
        e->wanted.output = h->output;
    }
    return e;
}

void om_enable_head(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* head) {
    OmConfig* c = om_config_of(resource);
    if (!om_unused(*c)) {
        return;
    }
    std::unique_ptr<OmConfigHead> e = om_new_entry(*c, head, true);
    if (e == nullptr) {
        return;
    }
    wl_resource* config_head =
        wl_resource_create(client, &zwlr_output_configuration_head_v1_interface,
                           wl_resource_get_version(resource), id);
    if (config_head == nullptr) {
        wl_resource_post_no_memory(resource);
        return;
    }
    e->resource = config_head;
    wl_resource_set_implementation(config_head, &om_config_head_impl, e.get(),
                                   om_config_head_resource_destroy);
    c->heads.push_back(std::move(e));
}

void om_disable_head(wl_client*, wl_resource* resource, wl_resource* head) {
    OmConfig* c = om_config_of(resource);
    if (!om_unused(*c)) {
        return;
    }
    if (std::unique_ptr<OmConfigHead> e = om_new_entry(*c, head, false); e != nullptr) {
        c->heads.push_back(std::move(e));
    }
}

void om_submit(wl_resource* resource, bool test) {
    OmConfig* c = om_config_of(resource);
    if (!om_unused(*c)) {
        return;
    }
    c->used = true;
    OmImpl* impl = c->impl;
    if (impl == nullptr || c->stale || c->serial != impl->serial) {
        zwlr_output_configuration_v1_send_cancelled(resource);
        return;
    }
    OutputConfigRequest request;
    request.test = test;
    request.heads.reserve(impl->heads.size());
    for (const auto& h : impl->heads) {
        auto it = std::ranges::find_if(c->heads, [&](const auto& e) { return e->head == h->id; });
        if (it == c->heads.end()) {
            wl_resource_post_error(resource, ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_UNCONFIGURED_HEAD,
                                   "head '%s' has not been configured", h->info.name.c_str());
            return;
        }
        request.heads.push_back((*it)->wanted);
    }
    impl->request.emit(request);
    switch (request.verdict) {
    case OutputConfigVerdict::succeeded:
        zwlr_output_configuration_v1_send_succeeded(resource);
        break;
    case OutputConfigVerdict::failed:
        zwlr_output_configuration_v1_send_failed(resource);
        break;
    case OutputConfigVerdict::cancelled:
        zwlr_output_configuration_v1_send_cancelled(resource);
        break;
    }
}

void om_apply(wl_client*, wl_resource* resource) { om_submit(resource, false); }
void om_test(wl_client*, wl_resource* resource) { om_submit(resource, true); }

constexpr struct zwlr_output_configuration_v1_interface om_config_impl = {
    .enable_head = om_enable_head,
    .disable_head = om_disable_head,
    .apply = om_apply,
    .test = om_test,
    .destroy = resource_destroy_request,
};

void om_config_resource_destroy(wl_resource* resource) {
    OmConfig* c = om_config_of(resource);
    // "This request also destroys wlr_output_configuration_head objects":
    // the client still holds them, so they go inert instead.
    for (const auto& e : c->heads) {
        if (e->resource != nullptr) {
            wl_resource_set_user_data(e->resource, nullptr);
        }
    }
    if (c->impl != nullptr) {
        std::erase(c->impl->configs, c);
    }
    delete c;
}

// --- zwlr_output_manager_v1 ---------------------------------------------------

void om_create_configuration(wl_client* client, wl_resource* resource, uint32_t id,
                             uint32_t serial) {
    auto* b = static_cast<OmBinding*>(wl_resource_get_user_data(resource));
    wl_resource* config = wl_resource_create(client, &zwlr_output_configuration_v1_interface,
                                             wl_resource_get_version(resource), id);
    if (config == nullptr) {
        wl_resource_post_no_memory(resource);
        return;
    }
    auto* c = new OmConfig;
    c->impl = b != nullptr ? b->impl : nullptr;
    c->resource = config;
    c->serial = serial;
    wl_resource_set_implementation(config, &om_config_impl, c, om_config_resource_destroy);
    if (c->impl != nullptr) {
        c->impl->configs.push_back(c);
    }
}

void om_stop(wl_client*, wl_resource* resource) {
    zwlr_output_manager_v1_send_finished(resource);
    wl_resource_destroy(resource);
}

constexpr struct zwlr_output_manager_v1_interface om_manager_impl = {
    .create_configuration = om_create_configuration,
    .stop = om_stop,
};

void om_manager_resource_destroy(wl_resource* resource) {
    auto* b = static_cast<OmBinding*>(wl_resource_get_user_data(resource));
    // Its heads stay client objects until released; they just stop hearing.
    for (OmHeadRes* r : b->heads) {
        r->binding = nullptr;
    }
    std::erase_if(b->impl->bindings,
                  [b](const std::unique_ptr<OmBinding>& held) { return held.get() == b; });
}

void om_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* impl = static_cast<OmImpl*>(data);
    wl_resource* resource = wl_resource_create(client, &zwlr_output_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto binding = std::make_unique<OmBinding>();
    binding->resource = resource;
    binding->impl = impl;
    OmBinding* b = binding.get();
    impl->bindings.push_back(std::move(binding));
    wl_resource_set_implementation(resource, &om_manager_impl, b, om_manager_resource_destroy);

    for (const auto& h : impl->heads) {
        impl->send_head(*b, *h);
    }
    zwlr_output_manager_v1_send_done(resource, impl->serial);
}

} // namespace

OutputManager::Impl::~Impl() {
    for (OmConfig* c : configs) {
        c->impl = nullptr; // an apply now is cancelled
    }
    for (const auto& b : bindings) {
        for (OmHeadRes* r : b->heads) {
            r->binding = nullptr;
        }
        wl_resource_set_user_data(b->resource, nullptr);
        wl_resource_set_destructor(b->resource, nullptr);
    }
}

OmHead* OutputManager::Impl::head(std::uint32_t id) const {
    for (const auto& h : heads) {
        if (h->id == id) {
            return h.get();
        }
    }
    return nullptr;
}

OmHeadState OutputManager::Impl::read(const Output& output) const {
    OmHeadState s;
    s.modes = output.modes();
    OutputMode current = output.current_mode();
    // A head with no mode list (headless, nested) still has a mode it runs at,
    // and current_mode can only name an advertised mode object.
    if (s.modes.empty()) {
        current.preferred = true;
    }
    auto it = std::ranges::find_if(s.modes, [&](const OutputMode& m) {
        return m.width == current.width && m.height == current.height &&
               m.refresh_mhz == current.refresh_mhz;
    });
    if (it == s.modes.end()) {
        s.modes.push_back(current);
        it = std::prev(s.modes.end());
    }
    s.current = static_cast<std::size_t>(it - s.modes.begin());

    const auto& placed = layout->outputs();
    const auto box =
        std::ranges::find_if(placed, [&](const OutputBox& e) { return e.output == &output; });
    s.enabled = box != placed.end();
    if (s.enabled) {
        s.x = box->box.x;
        s.y = box->box.y;
    }
    s.transform = output.transform();
    s.scale = output.scale();
    s.adaptive_sync = output.vrr_enabled();
    return s;
}

void OutputManager::Impl::send_head(OmBinding& b, const OmHead& h) {
    wl_client* client = wl_resource_get_client(b.resource);
    const int version = wl_resource_get_version(b.resource);
    wl_resource* resource = wl_resource_create(client, &zwlr_output_head_v1_interface, version, 0);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* r = new OmHeadRes{&b, resource, h.id, {}};
    wl_resource_set_implementation(resource, &om_head_impl, r, om_head_resource_destroy);
    b.heads.push_back(r);

    zwlr_output_manager_v1_send_head(b.resource, resource);
    zwlr_output_head_v1_send_name(resource, h.info.name.c_str());
    zwlr_output_head_v1_send_description(
        resource, (h.info.description.empty() ? h.info.name : h.info.description).c_str());
    if (h.info.physical_width_mm > 0 && h.info.physical_height_mm > 0) {
        zwlr_output_head_v1_send_physical_size(resource, h.info.physical_width_mm,
                                               h.info.physical_height_mm);
    }
    if (version >= ZWLR_OUTPUT_HEAD_V1_MAKE_SINCE_VERSION) {
        if (!h.info.make.empty()) {
            zwlr_output_head_v1_send_make(resource, h.info.make.c_str());
        }
        if (!h.info.model.empty()) {
            zwlr_output_head_v1_send_model(resource, h.info.model.c_str());
        }
        if (!h.info.serial_number.empty()) {
            zwlr_output_head_v1_send_serial_number(resource, h.info.serial_number.c_str());
        }
    }
    send_modes(*r, h.sent.modes);
    send_state(*r, h.sent, kOmAll);
}

void OutputManager::Impl::send_modes(OmHeadRes& r, const std::vector<OutputMode>& modes) {
    wl_client* client = wl_resource_get_client(r.resource);
    // Same version as the head, as wlroots does, although the mode interface
    // stops at 3: nothing past version 3 changed it.
    const int version = wl_resource_get_version(r.resource);
    for (const OutputMode& mode : modes) {
        wl_resource* resource =
            wl_resource_create(client, &zwlr_output_mode_v1_interface, version, 0);
        if (resource == nullptr) {
            wl_client_post_no_memory(client);
            r.modes.push_back(nullptr);
            continue;
        }
        auto* m = new OmModeRes{&r, resource, mode};
        wl_resource_set_implementation(resource, &om_mode_impl, m, om_mode_resource_destroy);
        r.modes.push_back(m);
        zwlr_output_head_v1_send_mode(r.resource, resource);
        zwlr_output_mode_v1_send_size(resource, mode.width, mode.height);
        if (mode.refresh_mhz > 0) {
            zwlr_output_mode_v1_send_refresh(resource, mode.refresh_mhz);
        }
        if (mode.preferred) {
            zwlr_output_mode_v1_send_preferred(resource);
        }
    }
}

void OutputManager::Impl::send_state(OmHeadRes& r, const OmHeadState& s, unsigned what) {
    if ((what & kOmEnabled) != 0) {
        zwlr_output_head_v1_send_enabled(r.resource, s.enabled ? 1 : 0);
    }
    if (!s.enabled) {
        return; // the rest is "only sent if the output is enabled"
    }
    if ((what & kOmCurrent) != 0 && s.current < r.modes.size() && r.modes[s.current] != nullptr) {
        zwlr_output_head_v1_send_current_mode(r.resource, r.modes[s.current]->resource);
    }
    if ((what & kOmPosition) != 0) {
        zwlr_output_head_v1_send_position(r.resource, s.x, s.y);
    }
    if ((what & kOmTransform) != 0) {
        zwlr_output_head_v1_send_transform(r.resource, static_cast<std::int32_t>(s.transform));
    }
    if ((what & kOmScale) != 0) {
        zwlr_output_head_v1_send_scale(r.resource, wl_fixed_from_int(s.scale));
    }
    if ((what & (kOmEnabled | kOmAdaptive)) != 0 &&
        wl_resource_get_version(r.resource) >= ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_SINCE_VERSION) {
        zwlr_output_head_v1_send_adaptive_sync(
            r.resource, s.adaptive_sync ? ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED
                                        : ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
    }
}

void OutputManager::Impl::finish_modes(OmHeadRes& r) {
    for (OmModeRes* m : r.modes) {
        if (m != nullptr) {
            zwlr_output_mode_v1_send_finished(m->resource);
            m->owner = nullptr;
        }
    }
    r.modes.clear();
}

void OutputManager::Impl::finish_head(OmHeadRes& r) {
    finish_modes(r);
    zwlr_output_head_v1_send_finished(r.resource);
    r.head = 0;
    if (r.binding != nullptr) {
        std::erase(r.binding->heads, &r);
        r.binding = nullptr;
    }
}

void OutputManager::Impl::remove(const Output& output) {
    auto it = std::ranges::find_if(heads, [&](const auto& h) { return h->output == &output; });
    if (it == heads.end()) {
        return;
    }
    const std::uint32_t id = (*it)->id;
    for (const auto& b : bindings) {
        // Copy: finishing a head takes it out of the binding's list.
        const std::vector<OmHeadRes*> live = b->heads;
        for (OmHeadRes* r : live) {
            if (r->head == id) {
                finish_head(*r);
            }
        }
    }
    // Safe from inside Output::destroy: a connection may drop itself mid-emit.
    heads.erase(it);
    send_done();
}

void OutputManager::Impl::send_done() {
    ++serial;
    for (const auto& b : bindings) {
        zwlr_output_manager_v1_send_done(b->resource, serial);
    }
}

OutputManager::OutputManager(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
OutputManager::~OutputManager() = default;
OutputManager::OutputManager(OutputManager&&) noexcept = default;
OutputManager& OutputManager::operator=(OutputManager&&) noexcept = default;

Signal<OutputConfigRequest>& OutputManager::request() noexcept { return impl_->request; }

Result<OutputManager> OutputManager::create(Display& display, const OutputLayout& layout) {
    auto impl = std::make_unique<Impl>();
    impl->layout = &layout;
    auto global = create_wl_global<&zwlr_output_manager_v1_interface, om_bind>(display, 4,
                                                                               impl.get());
    if (!global) {
        return fail(std::move(global.error().message));
    }
    impl->global = std::move(*global);
    return OutputManager{std::move(impl)};
}

void OutputManager::add_head(Output& output, OutputHeadInfo info) {
    Impl& self = *impl_;
    if (std::ranges::any_of(self.heads, [&](const auto& h) { return h->output == &output; })) {
        return;
    }
    auto owned = std::make_unique<OmHead>();
    OmHead& h = *owned;
    h.id = self.next_id++;
    h.output = &output;
    h.info = std::move(info);
    h.sent = self.read(output);
    Impl* impl = impl_.get(); // stable across moves of the OutputManager
    h.on_destroy =
        output.destroy.connect([impl, &output](OutputDestroy&) { impl->remove(output); });
    self.heads.push_back(std::move(owned));
    for (const auto& b : self.bindings) {
        self.send_head(*b, h);
    }
    self.send_done();
}

void OutputManager::remove_head(const Output& output) { impl_->remove(output); }

void OutputManager::update() {
    Impl& self = *impl_;
    bool changed = false;
    for (const auto& owned : self.heads) {
        OmHead& h = *owned;
        OmHeadState now = self.read(*h.output);
        const bool modes_changed = now.modes != h.sent.modes;
        unsigned what = 0;
        if (now.enabled != h.sent.enabled) {
            what = kOmAll; // a head that just came on has no state on record
        }
        if (modes_changed || now.current != h.sent.current) {
            what |= kOmCurrent; // a new mode list means new objects to point at
        }
        if (now.x != h.sent.x || now.y != h.sent.y) {
            what |= kOmPosition;
        }
        if (now.transform != h.sent.transform) {
            what |= kOmTransform;
        }
        if (now.scale != h.sent.scale) {
            what |= kOmScale;
        }
        if (now.adaptive_sync != h.sent.adaptive_sync) {
            what |= kOmAdaptive;
        }
        if (!modes_changed && what == 0) {
            continue;
        }
        changed = true;
        for (const auto& b : self.bindings) {
            for (OmHeadRes* r : b->heads) {
                if (r->head != h.id) {
                    continue;
                }
                if (modes_changed) {
                    self.finish_modes(*r);
                    self.send_modes(*r, now.modes);
                }
                self.send_state(*r, now, what);
            }
        }
        h.sent = std::move(now);
    }
    if (changed) {
        self.send_done();
    }
}

} // namespace luminaria
