// luminaria/util/keymap.cppm — a small RAII wrapper around xkb: one compiled
// keymap plus the live state machine over it. Backends and compositors share
// it so the xkb refcounting, the "evdev code + 8" offset and the modifier
// serialization live in exactly one place instead of being re-derived per
// consumer. It answers keyboard STATE only — keysyms and modifier masks.
// Bindings and actions are the compositor's job and deliberately absent.

module;

#include <cstdint>

#include <xkbcommon/xkbcommon.h>

export module luminaria:keymap;

import std;

import :expected;
import :input_event;

export namespace luminaria {

/// A compiled xkb keymap and the state over it. Move-only, RAII: every xkb
/// handle is unref'd on destruction or replacement.
class KeymapState {
public:
    /// Compile `layout` — an xkb layout name such as "us", or empty for the
    /// environment's XKB_DEFAULT_* settings. The compiled keymap text is kept
    /// for forwarding to clients.
    [[nodiscard]] static Result<KeymapState> from_layout(std::string_view layout = {});

    /// Compile a keymap from its text form — the payload wl_keyboard.keymap
    /// carries, e.g. a nested compositor forwarding its parent's keymap.
    [[nodiscard]] static Result<KeymapState> from_text(std::string_view text);

    ~KeymapState();
    KeymapState(KeymapState&&) noexcept;
    KeymapState& operator=(KeymapState&&) noexcept;
    KeymapState(const KeymapState&) = delete;
    KeymapState& operator=(const KeymapState&) = delete;

    /// The keymap as text — send this to clients (wl_keyboard.keymap).
    [[nodiscard]] const std::string& text() const noexcept;

    /// Feed one key event. `evdev_code` is the raw evdev keycode (KEY_ESC == 1);
    /// the xkb offset is applied here. Updates the modifier masks.
    void update_key(std::uint32_t evdev_code, bool pressed) noexcept;

    /// Adopt externally-observed modifier state wholesale — a nested compositor
    /// whose host changed focus while a modifier was held, or any feed that is
    /// not derived from the key events this state saw. The masks and layout
    /// group replace the current depressed/latched/locked state, so the next
    /// `keysym` is computed against what the rest of the world is actually
    /// holding.
    void update_modifiers(ModifiersEvent modifiers) noexcept;

    /// The keysym `evdev_code` produces right now, honouring the current layout
    /// and modifier state (so shift+key answers capital, dead keys compose).
    /// 0 when nothing sensible maps.
    [[nodiscard]] std::uint32_t keysym(std::uint32_t evdev_code) const noexcept;

    /// Current modifier masks and layout group, as wl_keyboard.modifiers
    /// carries them.
    [[nodiscard]] ModifiersEvent modifiers() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit KeymapState(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

struct KeymapState::Impl {
    xkb_context* ctx = nullptr;
    xkb_keymap* map = nullptr;
    xkb_state* state = nullptr;
    std::string text;

    ~Impl() {
        if (state != nullptr) {
            xkb_state_unref(state);
        }
        if (map != nullptr) {
            xkb_keymap_unref(map);
        }
        if (ctx != nullptr) {
            xkb_context_unref(ctx);
        }
    }
};

KeymapState::KeymapState(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
KeymapState::~KeymapState() = default;
KeymapState::KeymapState(KeymapState&&) noexcept = default;
KeymapState& KeymapState::operator=(KeymapState&&) noexcept = default;

Result<KeymapState> KeymapState::from_layout(std::string_view layout) {
    auto impl = std::make_unique<Impl>();
    impl->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (impl->ctx == nullptr) {
        return fail("xkb_context_new failed");
    }
    xkb_rule_names names{};
    if (!layout.empty()) {
        names.layout = layout.data(); // borrowed only for the call below
    }
    impl->map = xkb_keymap_new_from_names(impl->ctx, layout.empty() ? nullptr : &names,
                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (impl->map == nullptr) {
        return fail("xkb_keymap_new_from_names failed");
    }
    std::unique_ptr<char, decltype(&std::free)> as_text{
        xkb_keymap_get_as_string(impl->map, XKB_KEYMAP_FORMAT_TEXT_V1), std::free};
    if (!as_text) {
        return fail("xkb keymap serialization failed");
    }
    impl->text = as_text.get();
    impl->state = xkb_state_new(impl->map);
    if (impl->state == nullptr) {
        return fail("xkb_state_new failed");
    }
    return KeymapState{std::move(impl)};
}

Result<KeymapState> KeymapState::from_text(std::string_view text) {
    if (text.empty()) {
        return fail("empty keymap text");
    }
    auto impl = std::make_unique<Impl>();
    impl->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (impl->ctx == nullptr) {
        return fail("xkb_context_new failed");
    }
    impl->map = xkb_keymap_new_from_string(impl->ctx, text.data(), XKB_KEYMAP_FORMAT_TEXT_V1,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (impl->map == nullptr) {
        return fail("xkb_keymap_new_from_string failed");
    }
    impl->text = text;
    impl->state = xkb_state_new(impl->map);
    if (impl->state == nullptr) {
        return fail("xkb_state_new failed");
    }
    return KeymapState{std::move(impl)};
}

const std::string& KeymapState::text() const noexcept { return impl_->text; }

void KeymapState::update_key(std::uint32_t evdev_code, bool pressed) noexcept {
    // libinput (and the evdev protocol generally) reports keycodes 8 below
    // xkb's.
    xkb_state_update_key(impl_->state, evdev_code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

void KeymapState::update_modifiers(ModifiersEvent modifiers) noexcept {
    // The masks are already in xkb's bit numbering; `group` is the effective
    // layout index, applied as the locked layout the way wl_keyboard.modifiers
    // round-trips through wlroots' deserialization. Base/latched layouts stay
    // untouched: an injected mask feed has no opinion about them.
    xkb_state_update_mask(impl_->state, modifiers.depressed, modifiers.latched,
                          modifiers.locked, 0, 0, modifiers.group);
}

std::uint32_t KeymapState::keysym(std::uint32_t evdev_code) const noexcept {
    return static_cast<std::uint32_t>(
        xkb_state_key_get_one_sym(impl_->state, evdev_code + 8));
}

ModifiersEvent KeymapState::modifiers() const noexcept {
    return ModifiersEvent{
        xkb_state_serialize_mods(impl_->state, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(impl_->state, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(impl_->state, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(impl_->state, XKB_STATE_LAYOUT_EFFECTIVE)};
}

} // namespace luminaria
