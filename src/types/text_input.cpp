module;

// Implements zwp_text_input_manager_v3 (version 1).
//
// Everything a client sets is pending until `commit`, at which point it becomes
// current and `TextInput::commit` fires. Everything we send is pending until
// `send_done()`. The protocol says the done serial must equal the number of
// commit requests received, so that is literally what `commits_` counts.
//
// Focus is not the client's to choose: it follows the seat's keyboard focus,
// and after `leave` the protocol requires us to ignore that object's requests
// until the next `enter`.

#include <memory>

#include <algorithm>
#include <cstdint>
#include <string>
#include <typeinfo>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "text-input-unstable-v3-protocol.h"

module luminaria;

namespace luminaria {

// External linkage: TextInputManager::Impl holds these.
class TextInputImpl;

struct TextInputManager::Impl {
    wl_global* global = nullptr;
    Seat* seat = nullptr;
    std::vector<TextInputImpl*> text_inputs;
    Surface* focus = nullptr;

    Signal<NewTextInput> new_text_input;
    Signal<SeatKeyboardFocus>::Connection focus_conn;
    // The focused Surface is a raw pointer, so we watch it ourselves. We cannot
    // rely on the seat telling us: it clears its focus from the same destroy
    // signal, and whichever of us runs first, sending `leave` naming a
    // wl_surface that is being torn down is not something to do.
    Signal<SurfaceDestroy>::Connection focus_gone;

    /// Move focus to `surface` (null = nowhere): leave everyone who had it,
    /// then enter everyone belonging to the new client.
    void set_focus(Surface* surface);
    /// The focused surface is going away. Same bookkeeping, no `leave` event.
    void drop_focus_silently();
    void leave_all(Surface& surface, bool send_event);

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

using Mgr = TextInputManager::Impl;

class TextInputImpl final : public TextInput {
public:
    TextInputImpl(Mgr* mgr, wl_resource* resource) : mgr_(mgr), resource_(resource) {}

    [[nodiscard]] Surface* focused_surface() const noexcept override { return focus_; }
    [[nodiscard]] bool enabled() const noexcept override { return enabled_; }
    [[nodiscard]] const std::string& surrounding_text() const noexcept override {
        return state_.surrounding;
    }
    [[nodiscard]] std::uint32_t surrounding_cursor() const noexcept override {
        return state_.cursor;
    }
    [[nodiscard]] std::uint32_t surrounding_anchor() const noexcept override {
        return state_.anchor;
    }
    [[nodiscard]] TextChangeCause text_change_cause() const noexcept override {
        return state_.cause;
    }
    [[nodiscard]] std::uint32_t content_hint() const noexcept override { return state_.hint; }
    [[nodiscard]] TextInputPurpose content_purpose() const noexcept override {
        return state_.purpose;
    }
    [[nodiscard]] const Box& cursor_rectangle() const noexcept override { return state_.cursor_box; }
    [[nodiscard]] std::uint32_t serial() const noexcept override { return commits_; }

    void send_preedit_string(const std::string& text, std::int32_t cursor_begin,
                             std::int32_t cursor_end) override {
        if (focus_ == nullptr) {
            return;
        }
        zwp_text_input_v3_send_preedit_string(resource_, text.empty() ? nullptr : text.c_str(),
                                              cursor_begin, cursor_end);
    }
    void send_commit_string(const std::string& text) override {
        if (focus_ == nullptr) {
            return;
        }
        zwp_text_input_v3_send_commit_string(resource_, text.c_str());
    }
    void send_delete_surrounding_text(std::uint32_t before, std::uint32_t after) override {
        if (focus_ == nullptr) {
            return;
        }
        zwp_text_input_v3_send_delete_surrounding_text(resource_, before, after);
    }
    void send_done() override {
        if (focus_ == nullptr) {
            return;
        }
        zwp_text_input_v3_send_done(resource_, commits_);
    }

    // --- focus, driven by the manager ---
    void enter(Surface& surface) {
        focus_ = &surface;
        zwp_text_input_v3_send_enter(resource_, surface.c_resource());
    }
    void leave(Surface& surface, bool send_event) {
        // The protocol resets the client to "disabled" on leave; if it was on,
        // the compositor has to hear about it or an input method stays open.
        const bool was_enabled = enabled_;
        focus_ = nullptr;
        enabled_ = false;
        pending_ = State{};
        pending_enable_ = false;
        pending_enable_set_ = false;
        if (send_event) {
            zwp_text_input_v3_send_leave(resource_, surface.c_resource());
        }
        if (was_enabled) {
            TextInputDisable event{*this};
            disable.emit(event);
        }
    }

    // --- pending state, applied by commit ---
    struct State {
        std::string surrounding;
        std::uint32_t cursor = 0;
        std::uint32_t anchor = 0;
        TextChangeCause cause = TextChangeCause::input_method;
        std::uint32_t hint = text_input_hint_none;
        TextInputPurpose purpose = TextInputPurpose::normal;
        Box cursor_box{};
    };

    void apply_commit() {
        ++commits_;
        // `enable` resets everything, including what we have been told since —
        // so a commit carrying an enable starts from a blank State.
        const bool was_enabled = enabled_;
        if (pending_enable_set_) {
            enabled_ = pending_enable_;
        }
        state_ = pending_;
        // Pending state survives a commit only in the sense that the client
        // re-sends it; the protocol says the new pending state is the default.
        pending_ = State{};
        pending_enable_set_ = false;

        if (!was_enabled && enabled_) {
            TextInputEnable event{*this};
            enable.emit(event);
        } else if (was_enabled && !enabled_) {
            TextInputDisable event{*this};
            disable.emit(event);
        }
        TextInputCommit commit_event{*this};
        commit.emit(commit_event);
    }

    Mgr* mgr_ = nullptr;
    wl_resource* resource_ = nullptr;
    Surface* focus_ = nullptr;
    bool enabled_ = false;
    std::uint32_t commits_ = 0;

    State state_;
    State pending_;
    bool pending_enable_ = false;
    bool pending_enable_set_ = false;
};

namespace {

TextInputImpl* text_input_of(wl_resource* resource) {
    return static_cast<TextInputImpl*>(wl_resource_get_user_data(resource));
}

// "After leave event, compositor must ignore requests from any text input
// instances until next enter event."
bool focused(wl_resource* resource) {
    return text_input_of(resource)->focus_ != nullptr;
}

void text_input_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void text_input_enable(wl_client*, wl_resource* resource) {
    if (!focused(resource)) {
        return;
    }
    TextInputImpl* ti = text_input_of(resource);
    ti->pending_ = TextInputImpl::State{}; // enable resets every field
    ti->pending_enable_ = true;
    ti->pending_enable_set_ = true;
}

void text_input_disable(wl_client*, wl_resource* resource) {
    if (!focused(resource)) {
        return;
    }
    TextInputImpl* ti = text_input_of(resource);
    ti->pending_enable_ = false;
    ti->pending_enable_set_ = true;
}

void text_input_set_surrounding_text(wl_client*, wl_resource* resource, const char* text,
                                     int32_t cursor, int32_t anchor) {
    if (!focused(resource)) {
        return;
    }
    TextInputImpl* ti = text_input_of(resource);
    ti->pending_.surrounding = text != nullptr ? text : "";
    ti->pending_.cursor = static_cast<uint32_t>(cursor);
    ti->pending_.anchor = static_cast<uint32_t>(anchor);
}

void text_input_set_text_change_cause(wl_client*, wl_resource* resource, uint32_t cause) {
    if (!focused(resource)) {
        return;
    }
    text_input_of(resource)->pending_.cause =
        cause == ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD ? TextChangeCause::input_method
                                                             : TextChangeCause::other;
}

void text_input_set_content_type(wl_client*, wl_resource* resource, uint32_t hint,
                                 uint32_t purpose) {
    if (!focused(resource)) {
        return;
    }
    TextInputImpl* ti = text_input_of(resource);
    ti->pending_.hint = hint;
    ti->pending_.purpose = static_cast<TextInputPurpose>(purpose);
}

void text_input_set_cursor_rectangle(wl_client*, wl_resource* resource, int32_t x, int32_t y,
                                     int32_t width, int32_t height) {
    if (!focused(resource)) {
        return;
    }
    text_input_of(resource)->pending_.cursor_box = Box{x, y, width, height};
}

void text_input_commit(wl_client*, wl_resource* resource) {
    if (!focused(resource)) {
        return;
    }
    text_input_of(resource)->apply_commit();
}

constexpr struct zwp_text_input_v3_interface text_input_impl = {
    .destroy = text_input_destroy_request,
    .enable = text_input_enable,
    .disable = text_input_disable,
    .set_surrounding_text = text_input_set_surrounding_text,
    .set_text_change_cause = text_input_set_text_change_cause,
    .set_content_type = text_input_set_content_type,
    .set_cursor_rectangle = text_input_set_cursor_rectangle,
    .commit = text_input_commit,
};

void text_input_resource_destroy(wl_resource* resource) {
    TextInputImpl* ti = text_input_of(resource);
    TextInputDestroy event{*ti};
    ti->destroy.emit(event);
    std::erase(ti->mgr_->text_inputs, ti);
    delete ti;
}

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_get_text_input(wl_client* client, wl_resource* manager_resource, uint32_t id,
                            wl_resource* /*seat*/) {
    // The seat argument picks which seat's focus to follow; luminaria has one.
    auto* mgr = static_cast<Mgr*>(wl_resource_get_user_data(manager_resource));
    wl_resource* resource =
        wl_resource_create(client, &zwp_text_input_v3_interface,
                           wl_resource_get_version(manager_resource), static_cast<int>(id));
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* ti = new TextInputImpl(mgr, resource);
    wl_resource_set_implementation(resource, &text_input_impl, ti, text_input_resource_destroy);
    mgr->text_inputs.push_back(ti);

    NewTextInput event{*ti};
    mgr->new_text_input.emit(event);

    // A client that binds while already focused must be told so, or it will sit
    // waiting for an enter that has already happened.
    if (mgr->focus != nullptr &&
        wl_resource_get_client(mgr->focus->c_resource()) == client) {
        ti->enter(*mgr->focus);
    }
}

constexpr struct zwp_text_input_manager_v3_interface manager_impl = {
    .destroy = manager_destroy_request,
    .get_text_input = manager_get_text_input,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_text_input_manager_v3_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

void TextInputManager::Impl::leave_all(Surface& surface, bool send_event) {
    // Copy first: leave() fires a signal whose handler may destroy a text input.
    std::vector<TextInputImpl*> targets;
    for (TextInputImpl* ti : text_inputs) {
        if (ti->focus_ == &surface) {
            targets.push_back(ti);
        }
    }
    for (TextInputImpl* ti : targets) {
        ti->leave(surface, send_event);
    }
}

void TextInputManager::Impl::set_focus(Surface* surface) {
    if (focus == surface) {
        return;
    }
    Surface* old = focus;
    focus = surface;
    focus_gone.disconnect();
    // Leave first, then enter — the protocol requires that order, and an input
    // method that saw them the other way round would attach to the wrong window.
    if (old != nullptr) {
        leave_all(*old, true);
    }
    if (surface != nullptr) {
        Impl* self = this;
        focus_gone =
            surface->destroy.connect([self](SurfaceDestroy&) { self->drop_focus_silently(); });
        wl_client* client = wl_resource_get_client(surface->c_resource());
        for (TextInputImpl* ti : text_inputs) {
            if (wl_resource_get_client(ti->resource_) == client) {
                ti->enter(*surface);
            }
        }
    }
}

void TextInputManager::Impl::drop_focus_silently() {
    if (focus == nullptr) {
        return;
    }
    Surface* old = focus;
    focus = nullptr;
    focus_gone.disconnect();
    leave_all(*old, false);
}

TextInputManager::TextInputManager(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TextInputManager::~TextInputManager() = default;
TextInputManager::TextInputManager(TextInputManager&&) noexcept = default;
TextInputManager& TextInputManager::operator=(TextInputManager&&) noexcept = default;

Result<TextInputManager> TextInputManager::create(Display& display, Seat& seat) {
    auto impl = std::make_unique<Impl>();
    impl->seat = &seat;
    impl->global = wl_global_create(display.c_ptr(), &zwp_text_input_manager_v3_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zwp_text_input_manager_v3) failed");
    }
    Impl* raw = impl.get();
    impl->focus_conn = seat.keyboard_focus_changed().connect(
        [raw](SeatKeyboardFocus& e) { raw->set_focus(e.surface); });
    return TextInputManager{std::move(impl)};
}

Signal<NewTextInput>& TextInputManager::new_text_input() noexcept {
    return impl_->new_text_input;
}

TextInput* TextInputManager::focused() noexcept {
    auto it = std::ranges::find_if(impl_->text_inputs, [](const TextInputImpl* ti) {
        return ti->focus_ != nullptr && ti->enabled_;
    });
    return it != impl_->text_inputs.end() ? *it : nullptr;
}

} // namespace luminaria
