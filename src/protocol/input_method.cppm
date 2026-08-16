// luminaria/input_method.cppm — zwp_input_method_manager_v2: the other end of
// text-input.
//
// `text-input-v3` is the application's side of typing CJK, emoji or anything
// else that is not one keystroke per character. This is the input method's:
// IBus, Fcitx or an on-screen keyboard connects as an ordinary Wayland client
// and asks for the seat's input method object. The compositor sits in the
// middle and copies state across — which is exactly what this file is.
//
// The bridge is complete and automatic. Create the global with the
// `TextInputManager` and the `Seat`, and:
//
//   * the focused text input enabling turns into `activate` + the surrounding
//     text, change cause and content type + `done`;
//   * every text-input `commit` re-sends that state;
//   * losing focus or disabling turns into `deactivate` + `done`;
//   * the input method's `commit_string` / `set_preedit_string` /
//     `delete_surrounding_text` are staged on the focused text input and
//     applied by its `commit`.
//
// Two things stay the compositor's:
//
//   * **The keyboard grab.** An input method that asks for one wants the raw
//     keys BEFORE the focused client sees them — that is how it swallows the
//     letters it converts. This library cannot do the routing for you, because
//     it is not the one holding the keyboard: check `keyboard_grab()` in your
//     key handler and, when it is non-null, feed it instead of the seat.
//   * **Positioning the popup.** `InputPopupSurface` is a surface to draw next
//     to the caret; the caret's box comes from the text input and is forwarded
//     to the client, but placing the window on screen is layout.
//
// An input method sees every keystroke and every field of every client, so this
// is a desktop-shell protocol: it lives in `luminaria.desktop` and is never in
// the default global list (ADR 0003).

module;

#include <cstdint>

#include <sys/mman.h>
#include <typeinfo>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "input-method-unstable-v2-protocol.h"

export module luminaria.desktop:input_method;

import std;

import luminaria;

export namespace luminaria {

class InputMethod;
class InputMethodKeyboardGrab;
class InputPopupSurface;

struct NewInputMethod {
    InputMethod& input_method;
};
struct InputMethodDestroy {
    InputMethod& input_method;
};
struct NewInputPopupSurface {
    InputPopupSurface& popup;
};
struct InputPopupSurfaceDestroy {
    InputPopupSurface& popup;
};
/// The input method took or dropped the keyboard grab. While `grabbed`, route
/// key events to `InputMethod::keyboard_grab()` instead of the seat.
struct InputMethodGrabChange {
    InputMethod& input_method;
    bool grabbed;
};

/// One zwp_input_method_keyboard_grab_v2. Owned by its resource; address stable.
/// The keymap is sent for you when the grab is taken; everything else is
/// forwarded by the compositor from whatever its keyboard source is.
class InputMethodKeyboardGrab {
public:
    virtual ~InputMethodKeyboardGrab() = default;
    InputMethodKeyboardGrab(const InputMethodKeyboardGrab&) = delete;
    InputMethodKeyboardGrab& operator=(const InputMethodKeyboardGrab&) = delete;

    /// `time` is a CLOCK_MONOTONIC millisecond stamp; `pressed` false is a release.
    virtual void send_key(std::uint32_t time, std::uint32_t key, bool pressed) = 0;
    virtual void send_modifiers(std::uint32_t depressed, std::uint32_t latched,
                                std::uint32_t locked, std::uint32_t group) = 0;
    virtual void send_repeat_info(int rate, int delay) = 0;

protected:
    InputMethodKeyboardGrab() = default;
};

/// One zwp_input_popup_surface_v2: the candidate window. Owned by its resource.
class InputPopupSurface {
public:
    virtual ~InputPopupSurface() = default;
    InputPopupSurface(const InputPopupSurface&) = delete;
    InputPopupSurface& operator=(const InputPopupSurface&) = delete;

    Signal<InputPopupSurfaceDestroy> destroy;

    /// The wl_surface to draw. Place it against the caret; the box below is in
    /// the TEXT INPUT's surface coordinates, so it is relative to wherever you
    /// placed that window.
    [[nodiscard]] virtual Surface& surface() noexcept = 0;
    /// The caret box last reported by the focused text input, or an empty box.
    [[nodiscard]] virtual const Box& text_input_rectangle() const noexcept = 0;

protected:
    InputPopupSurface() = default;
};

/// One zwp_input_method_v2: an input method's connection to one seat. Owned by
/// its resource; address stable, so signals may capture a reference.
class InputMethod {
public:
    virtual ~InputMethod() = default;
    InputMethod(const InputMethod&) = delete;
    InputMethod& operator=(const InputMethod&) = delete;

    Signal<NewInputPopupSurface> new_popup_surface;
    Signal<InputMethodGrabChange> grab_changed;
    Signal<InputMethodDestroy> destroy;

    /// True between `activate` and `deactivate`: a text field is taking input.
    [[nodiscard]] virtual bool active() const noexcept = 0;
    /// The keyboard grab this input method holds, or null. Feed key events here
    /// instead of to the seat while it is non-null.
    [[nodiscard]] virtual InputMethodKeyboardGrab* keyboard_grab() noexcept = 0;
    /// Its candidate windows, in creation order.
    [[nodiscard]] virtual const std::vector<InputPopupSurface*>& popup_surfaces()
        const noexcept = 0;

protected:
    InputMethod() = default;
};

/// The zwp_input_method_manager_v2 global (version 1). Move-only;
/// pointer-stable state.
class InputMethodManager {
public:
    /// Bridge `text_inputs` to whatever input method connects. `seat` supplies
    /// the keymap for keyboard grabs, and is the seat the manager answers for.
    [[nodiscard]] static Result<InputMethodManager> create(Display& display, Seat& seat,
                                                           TextInputManager& text_inputs);

    ~InputMethodManager();
    InputMethodManager(InputMethodManager&&) noexcept;
    InputMethodManager& operator=(InputMethodManager&&) noexcept;
    InputMethodManager(const InputMethodManager&) = delete;
    InputMethodManager& operator=(const InputMethodManager&) = delete;

    [[nodiscard]] Signal<NewInputMethod>& new_input_method() noexcept;

    /// The input method bound to the seat, or null. At most one at a time: a
    /// second client asking is told `unavailable` and gets an inert object.
    [[nodiscard]] InputMethod* current() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit InputMethodManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
// Implements zwp_input_method_manager_v2 (version 1) and bridges it to
// zwp_text_input_v3. Everything the input method sends is staged on the text
// input and applied by its `commit` request, so the two protocols'
// double-buffering lines up one-to-one.

namespace luminaria {

// Not in an anonymous namespace: InputMethodManager::Impl holds these.
class InputMethodImpl;
class GrabImpl;
class PopupImpl;

struct InputMethodManager::Impl {
    wl_global* global = nullptr;
    wl_display* display = nullptr;
    Seat* seat = nullptr;
    TextInputManager* text_inputs = nullptr;
    InputMethodImpl* current = nullptr;

    Signal<NewInputMethod> new_input_method;
    Signal<NewTextInput>::Connection on_new_text_input;
    std::vector<Signal<TextInputEnable>::Connection> on_enable;
    std::vector<Signal<TextInputDisable>::Connection> on_disable;
    std::vector<Signal<TextInputCommit>::Connection> on_commit;

    /// Copy the focused text input's state to the input method and send `done`.
    void push_state(bool activating);
    void deactivate();

    ~Impl() {
        if (global != nullptr) {
            wl_global_destroy(global);
        }
    }
};

using ImMgr = InputMethodManager::Impl;

class GrabImpl final : public InputMethodKeyboardGrab {
public:
    GrabImpl(ImMgr* mgr, InputMethodImpl* im, wl_resource* resource)
        : mgr_(mgr), im_(im), resource_(resource) {}

    void send_key(std::uint32_t time, std::uint32_t key, bool pressed) override {
        zwp_input_method_keyboard_grab_v2_send_key(
            resource_, wl_display_next_serial(mgr_->display), time, key,
            pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    void send_modifiers(std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
                        std::uint32_t group) override {
        zwp_input_method_keyboard_grab_v2_send_modifiers(
            resource_, wl_display_next_serial(mgr_->display), depressed, latched, locked, group);
    }
    void send_repeat_info(int rate, int delay) override {
        zwp_input_method_keyboard_grab_v2_send_repeat_info(resource_, rate, delay);
    }

    ImMgr* mgr_ = nullptr;
    InputMethodImpl* im_ = nullptr;
    wl_resource* resource_ = nullptr;
};

class PopupImpl final : public InputPopupSurface {
public:
    PopupImpl(InputMethodImpl* im, Surface& surface, wl_resource* resource)
        : im_(im), surface_(&surface), resource_(resource) {
        on_surface_destroy_ =
            surface.destroy.connect([this](SurfaceDestroy&) { surface_ = nullptr; });
    }

    Surface& surface() noexcept override { return *surface_; }
    [[nodiscard]] const Box& text_input_rectangle() const noexcept override { return rect_; }

    void send_rectangle(const Box& rect) {
        if (rect == rect_) {
            return;
        }
        rect_ = rect;
        zwp_input_popup_surface_v2_send_text_input_rectangle(resource_, rect.x, rect.y,
                                                            rect.width, rect.height);
    }

    InputMethodImpl* im_ = nullptr;
    Surface* surface_ = nullptr;
    wl_resource* resource_ = nullptr;
    Box rect_{};
    Signal<SurfaceDestroy>::Connection on_surface_destroy_;
};

class InputMethodImpl final : public InputMethod {
public:
    InputMethodImpl(ImMgr* mgr, wl_resource* resource) : mgr_(mgr), resource_(resource) {}

    [[nodiscard]] bool active() const noexcept override { return active_; }
    [[nodiscard]] InputMethodKeyboardGrab* keyboard_grab() noexcept override { return grab_; }
    [[nodiscard]] const std::vector<InputPopupSurface*>& popup_surfaces() const noexcept override {
        return popups_;
    }

    void set_grab(GrabImpl* grab) {
        grab_ = grab;
        InputMethodGrabChange event{*this, grab != nullptr};
        grab_changed.emit(event);
    }

    ImMgr* mgr_ = nullptr;
    wl_resource* resource_ = nullptr;
    bool active_ = false;
    /// Counts the `done` events sent. The input method echoes it in `commit`;
    /// a mismatch means it answered a state we have already moved past.
    std::uint32_t done_count_ = 0;
    GrabImpl* grab_ = nullptr;
    std::vector<InputPopupSurface*> popups_;
};

namespace {

/// The text input an input method's requests apply to: the focused, enabled
/// one, or null when there is none.
TextInput* target(ImMgr* mgr) {
    return mgr->text_inputs != nullptr ? mgr->text_inputs->focused() : nullptr;
}

// --- zwp_input_method_keyboard_grab_v2 ---

void grab_release(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct zwp_input_method_keyboard_grab_v2_interface grab_impl = {
    .release = grab_release,
};

void grab_resource_destroy(wl_resource* resource) {
    auto* grab = static_cast<GrabImpl*>(wl_resource_get_user_data(resource));
    if (grab->im_ != nullptr) {
        grab->im_->set_grab(nullptr);
    }
    delete grab;
}

/// Hand the client the seat's keymap through a memfd, the way wl_keyboard does.
/// An input method without a keymap cannot tell which key was pressed.
void send_keymap(ImMgr* mgr, wl_resource* grab) {
    if (mgr->seat == nullptr) {
        return;
    }
    const std::string& text = mgr->seat->keymap();
    const size_t size = text.size() + 1; // the NUL is part of the payload
    int fd = memfd_create("luminaria-im-keymap", MFD_CLOEXEC);
    if (fd < 0) {
        return;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) == 0) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            std::memcpy(p, text.c_str(), size);
            munmap(p, size);
            zwp_input_method_keyboard_grab_v2_send_keymap(
                grab, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<uint32_t>(size));
            zwp_input_method_keyboard_grab_v2_send_repeat_info(grab, 25, 600);
        }
    }
    close(fd);
}

// --- zwp_input_popup_surface_v2 ---

void popup_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct zwp_input_popup_surface_v2_interface popup_impl = {
    .destroy = popup_destroy_request,
};

void popup_resource_destroy(wl_resource* resource) {
    auto* popup = static_cast<PopupImpl*>(wl_resource_get_user_data(resource));
    InputPopupSurfaceDestroy event{*popup};
    popup->destroy.emit(event);
    if (popup->im_ != nullptr) {
        std::erase(popup->im_->popups_, static_cast<InputPopupSurface*>(popup));
    }
    delete popup;
}

// --- zwp_input_method_v2 ---

void im_commit_string(wl_client*, wl_resource* resource, const char* text) {
    auto* im = static_cast<InputMethodImpl*>(wl_resource_get_user_data(resource));
    if (TextInput* ti = target(im->mgr_); ti != nullptr) {
        ti->send_commit_string(text != nullptr ? text : "");
    }
}

void im_set_preedit_string(wl_client*, wl_resource* resource, const char* text,
                           int32_t cursor_begin, int32_t cursor_end) {
    auto* im = static_cast<InputMethodImpl*>(wl_resource_get_user_data(resource));
    if (TextInput* ti = target(im->mgr_); ti != nullptr) {
        ti->send_preedit_string(text != nullptr ? text : "", cursor_begin, cursor_end);
    }
}

void im_delete_surrounding_text(wl_client*, wl_resource* resource, uint32_t before_length,
                                uint32_t after_length) {
    auto* im = static_cast<InputMethodImpl*>(wl_resource_get_user_data(resource));
    if (TextInput* ti = target(im->mgr_); ti != nullptr) {
        ti->send_delete_surrounding_text(before_length, after_length);
    }
}

void im_commit(wl_client*, wl_resource* resource, uint32_t serial) {
    auto* im = static_cast<InputMethodImpl*>(wl_resource_get_user_data(resource));
    if (serial != im->done_count_) {
        // Computed against state we have since replaced. The protocol says to
        // ignore it; the staged strings go with it, since the next `done` we
        // send will make the input method compose again from current state.
        return;
    }
    if (TextInput* ti = target(im->mgr_); ti != nullptr) {
        ti->send_done();
    }
}

void im_get_input_popup_surface(wl_client* client, wl_resource* resource, uint32_t id,
                                wl_resource* surface_resource) {
    auto* im = static_cast<InputMethodImpl*>(wl_resource_get_user_data(resource));
    Surface* surface = surface_from_resource(surface_resource);
    wl_resource* popup_resource =
        wl_resource_create(client, &zwp_input_popup_surface_v2_interface,
                           wl_resource_get_version(resource), static_cast<int>(id));
    if (popup_resource == nullptr) {
        wl_resource_post_no_memory(resource);
        return;
    }
    if (surface == nullptr) {
        wl_resource_set_implementation(popup_resource, &popup_impl, nullptr, nullptr);
        return;
    }
    auto* popup = new PopupImpl(im, *surface, popup_resource);
    wl_resource_set_implementation(popup_resource, &popup_impl, popup, popup_resource_destroy);
    im->popups_.push_back(popup);

    // Tell it where the caret is straight away, if we know.
    if (TextInput* ti = target(im->mgr_); ti != nullptr) {
        popup->send_rectangle(ti->cursor_rectangle());
    }
    NewInputPopupSurface event{*popup};
    im->new_popup_surface.emit(event);
}

void im_grab_keyboard(wl_client* client, wl_resource* resource, uint32_t keyboard) {
    auto* im = static_cast<InputMethodImpl*>(wl_resource_get_user_data(resource));
    wl_resource* grab_resource =
        wl_resource_create(client, &zwp_input_method_keyboard_grab_v2_interface,
                           wl_resource_get_version(resource), static_cast<int>(keyboard));
    if (grab_resource == nullptr) {
        wl_resource_post_no_memory(resource);
        return;
    }
    auto* grab = new GrabImpl(im->mgr_, im, grab_resource);
    wl_resource_set_implementation(grab_resource, &grab_impl, grab, grab_resource_destroy);
    send_keymap(im->mgr_, grab_resource);
    im->set_grab(grab);
}

void im_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

constexpr struct zwp_input_method_v2_interface im_impl = {
    .commit_string = im_commit_string,
    .set_preedit_string = im_set_preedit_string,
    .delete_surrounding_text = im_delete_surrounding_text,
    .commit = im_commit,
    .get_input_popup_surface = im_get_input_popup_surface,
    .grab_keyboard = im_grab_keyboard,
    .destroy = im_destroy_request,
};

void im_resource_destroy(wl_resource* resource) {
    auto* im = static_cast<InputMethodImpl*>(wl_resource_get_user_data(resource));
    InputMethodDestroy event{*im};
    im->destroy.emit(event);
    // The popups and the grab are the client's resources and die with it; they
    // only need to stop pointing at a freed input method.
    for (InputPopupSurface* popup : im->popups_) {
        static_cast<PopupImpl*>(popup)->im_ = nullptr;
    }
    if (im->grab_ != nullptr) {
        im->grab_->im_ = nullptr;
    }
    if (im->mgr_->current == im) {
        im->mgr_->current = nullptr;
    }
    delete im;
}

// --- zwp_input_method_manager_v2 ---

void manager_destroy_request(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void manager_get_input_method(wl_client* client, wl_resource* manager, wl_resource*,
                              uint32_t input_method) {
    auto* mgr = static_cast<ImMgr*>(wl_resource_get_user_data(manager));
    wl_resource* resource = wl_resource_create(client, &zwp_input_method_v2_interface,
                                               wl_resource_get_version(manager),
                                               static_cast<int>(input_method));
    if (resource == nullptr) {
        wl_resource_post_no_memory(manager);
        return;
    }
    if (mgr->current != nullptr) {
        // One input method per seat. The loser gets a live object that will
        // never activate, which is what `unavailable` means.
        wl_resource_set_implementation(resource, &im_impl, nullptr, nullptr);
        zwp_input_method_v2_send_unavailable(resource);
        return;
    }
    auto* im = new InputMethodImpl(mgr, resource);
    wl_resource_set_implementation(resource, &im_impl, im, im_resource_destroy);
    mgr->current = im;

    NewInputMethod event{*im};
    mgr->new_input_method.emit(event);
    // A field may already be focused and enabled — an input method started
    // mid-session must not have to wait for the user to click elsewhere.
    mgr->push_state(true);
}

constexpr struct zwp_input_method_manager_v2_interface manager_impl = {
    .get_input_method = manager_get_input_method,
    .destroy = manager_destroy_request,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_input_method_manager_v2_interface,
                                               static_cast<int>(version), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

} // namespace

void InputMethodManager::Impl::push_state(bool activating) {
    InputMethodImpl* im = current;
    if (im == nullptr) {
        return;
    }
    TextInput* ti = target(this);
    if (ti == nullptr) {
        deactivate();
        return;
    }
    if (activating && !im->active_) {
        im->active_ = true;
        zwp_input_method_v2_send_activate(im->resource_);
    }
    if (!im->active_) {
        return;
    }
    zwp_input_method_v2_send_surrounding_text(im->resource_, ti->surrounding_text().c_str(),
                                              ti->surrounding_cursor(), ti->surrounding_anchor());
    zwp_input_method_v2_send_text_change_cause(
        im->resource_, static_cast<std::uint32_t>(ti->text_change_cause()));
    zwp_input_method_v2_send_content_type(im->resource_, ti->content_hint(),
                                          static_cast<std::uint32_t>(ti->content_purpose()));
    zwp_input_method_v2_send_done(im->resource_);
    ++im->done_count_;

    // The caret moved with the state; the candidate window follows it.
    for (InputPopupSurface* popup : im->popups_) {
        static_cast<PopupImpl*>(popup)->send_rectangle(ti->cursor_rectangle());
    }
}

void InputMethodManager::Impl::deactivate() {
    InputMethodImpl* im = current;
    if (im == nullptr || !im->active_) {
        return;
    }
    im->active_ = false;
    zwp_input_method_v2_send_deactivate(im->resource_);
    zwp_input_method_v2_send_done(im->resource_);
    ++im->done_count_;
}

InputMethodManager::InputMethodManager(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
InputMethodManager::~InputMethodManager() = default;
InputMethodManager::InputMethodManager(InputMethodManager&&) noexcept = default;
InputMethodManager& InputMethodManager::operator=(InputMethodManager&&) noexcept = default;

Result<InputMethodManager> InputMethodManager::create(Display& display, Seat& seat,
                                                      TextInputManager& text_inputs) {
    auto impl = std::make_unique<Impl>();
    impl->display = display.c_ptr();
    impl->seat = &seat;
    impl->text_inputs = &text_inputs;
    impl->global = wl_global_create(display.c_ptr(), &zwp_input_method_manager_v2_interface, 1,
                                    impl.get(), manager_bind);
    if (impl->global == nullptr) {
        return fail("wl_global_create(zwp_input_method_manager_v2) failed");
    }

    // Every text input, present and future, is watched: which one is focused
    // changes under us, and the input method only ever hears about that one.
    Impl* raw = impl.get();
    raw->on_new_text_input = text_inputs.new_text_input().connect([raw](NewTextInput& event) {
        TextInput& ti = event.text_input;
        raw->on_enable.push_back(
            ti.enable.connect([raw](TextInputEnable&) { raw->push_state(true); }));
        raw->on_disable.push_back(
            ti.disable.connect([raw](TextInputDisable&) { raw->deactivate(); }));
        raw->on_commit.push_back(
            ti.commit.connect([raw](TextInputCommit&) { raw->push_state(true); }));
    });
    return InputMethodManager{std::move(impl)};
}

Signal<NewInputMethod>& InputMethodManager::new_input_method() noexcept {
    return impl_->new_input_method;
}

InputMethod* InputMethodManager::current() noexcept {
    return impl_->current;
}

} // namespace luminaria
