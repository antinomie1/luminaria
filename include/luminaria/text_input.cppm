// luminaria/text_input.cppm — zwp_text_input_manager_v3: typing text that is
// not a keystroke.
//
// A CJK input method, an emoji picker or an on-screen keyboard cannot express
// itself as wl_keyboard events: "ni hao" is five keystrokes that become two
// characters, and the client has to show the half-finished conversion while it
// happens. text-input-v3 is the channel for that. Two directions:
//
//   * The client tells us its state — that it wants input at all (`enable`),
//     the text around the cursor, what kind of field it is (a password? a URL?),
//     and where the cursor is ON SCREEN so a candidate window can be put next
//     to it. All of it is DOUBLE-BUFFERED: the setters only stage, and `commit`
//     applies the lot at once, exactly like wl_surface. `commit` is the signal
//     to act on; the individual setters have no signal of their own.
//   * We send text back — `preedit` is the uncommitted, usually underlined
//     conversion in progress, `commit_string` is text that is now really there,
//     and `delete_surrounding_text` removes what the composition replaces. They
//     are staged too: nothing reaches the client until `send_done()`.
//
// This library terminates the protocol; it does not implement an input method.
// What it gives you is a `TextInput` per client object, focus already tracked
// against the seat, and the send_* methods. Bridging those to IBus, Fcitx or
// an input-method-v2 global is the compositor's job.
//
// Focus follows the seat's KEYBOARD focus, and at most one text input is
// focused at a time — so keep the Seat alive at least as long as this.

module;

#include <cstdint>
#include <memory>
#include <string>

export module luminaria:text_input;

import :core.expected;
import :core.signal;
import :util.box;

export namespace luminaria {

class Display;
class Seat;
class Surface;
class TextInput;

/// Why the surrounding text changed, so an input method can tell its own edit
/// from the user moving the caret.
enum class TextChangeCause : std::uint32_t {
    /// The input method's own commit did it — nothing to react to.
    input_method = 0,
    /// The user (or the application) did: abandon any composition in progress.
    other = 1,
};

/// What sort of field this is. Drives autocapitalisation, the on-screen
/// keyboard's layout, and whether a candidate window should appear at all.
enum class TextInputPurpose : std::uint32_t {
    normal = 0,
    alpha = 1,
    digits = 2,
    number = 3,
    phone = 4,
    url = 5,
    email = 6,
    name = 7,
    password = 8,
    pin = 9,
    date = 10,
    time = 11,
    datetime = 12,
    terminal = 13,
};

/// Bitmask of behaviour hints (values match `zwp_text_input_v3.content_hint`).
enum TextInputHint : std::uint32_t {
    text_input_hint_none = 0,
    text_input_hint_completion = 1,
    text_input_hint_spellcheck = 2,
    text_input_hint_auto_capitalization = 4,
    text_input_hint_lowercase = 8,
    text_input_hint_uppercase = 16,
    text_input_hint_titlecase = 32,
    text_input_hint_hidden_text = 64,
    text_input_hint_sensitive_data = 128,
    text_input_hint_latin = 256,
    text_input_hint_multiline = 512,
};

struct NewTextInput {
    TextInput& text_input;
};
/// The client turned text input on for the focused surface. Start the input
/// method. Note that `enable` resets every piece of state to its default, so
/// read the state in `commit`, not here.
struct TextInputEnable {
    TextInput& text_input;
};
/// The client turned it off (or lost focus). Stop the input method and drop any
/// composition in progress — the client has already forgotten the preedit.
struct TextInputDisable {
    TextInput& text_input;
};
/// A `commit` applied the staged state. This is the only place the getters below
/// are meaningful.
struct TextInputCommit {
    TextInput& text_input;
};
struct TextInputDestroy {
    TextInput& text_input;
};

/// One zwp_text_input_v3. Owned by its resource; address stable for its
/// lifetime, so signals may capture `TextInput&`.
class TextInput {
public:
    virtual ~TextInput() = default;
    TextInput(const TextInput&) = delete;
    TextInput& operator=(const TextInput&) = delete;

    Signal<TextInputEnable> enable;
    Signal<TextInputDisable> disable;
    Signal<TextInputCommit> commit;
    Signal<TextInputDestroy> destroy;

    /// The surface this object is currently focused on, or null. Managed here
    /// from the seat's keyboard focus; the client never chooses it.
    [[nodiscard]] virtual Surface* focused_surface() const noexcept = 0;
    /// True between `enable` and `disable`. A disabled text input is still a
    /// live object — clients keep theirs around for the life of the window.
    [[nodiscard]] virtual bool enabled() const noexcept = 0;

    // --- current (committed) state ---

    /// Text around the cursor, UTF-8, empty if the client does not send it (it
    /// is optional, and a password field will not). `cursor` and `anchor` are
    /// BYTE offsets into it; they are equal when nothing is selected.
    [[nodiscard]] virtual const std::string& surrounding_text() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t surrounding_cursor() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t surrounding_anchor() const noexcept = 0;
    [[nodiscard]] virtual TextChangeCause text_change_cause() const noexcept = 0;
    /// Bitmask of TextInputHint.
    [[nodiscard]] virtual std::uint32_t content_hint() const noexcept = 0;
    [[nodiscard]] virtual TextInputPurpose content_purpose() const noexcept = 0;
    /// Where to put the candidate window: the caret's box in SURFACE
    /// coordinates. Zero-sized until the client says otherwise.
    [[nodiscard]] virtual const Box& cursor_rectangle() const noexcept = 0;

    // --- what we send back (staged until send_done) ---

    /// The composition in progress, to be drawn at the cursor as provisional
    /// text. `cursor_begin`/`cursor_end` are byte offsets into `text` marking
    /// the selected part, or both -1 to hide the cursor inside the preedit.
    /// An empty `text` clears it.
    virtual void send_preedit_string(const std::string& text, std::int32_t cursor_begin,
                                     std::int32_t cursor_end) = 0;
    /// Text that is now really in the field, replacing any preedit.
    virtual void send_commit_string(const std::string& text) = 0;
    /// Delete `before`/`after` BYTES around the cursor before applying the
    /// commit string — how a composition swallows the letters that produced it.
    virtual void send_delete_surrounding_text(std::uint32_t before, std::uint32_t after) = 0;
    /// Apply everything staged since the last done, and bump the serial. Nothing
    /// above takes effect without it. Sending done with nothing staged is how
    /// you clear a preedit.
    virtual void send_done() = 0;

    /// The serial sent with `done`: the number of `commit` requests this object
    /// has received. The client compares it with its own count to tell whether
    /// our answer was computed against the state it has now, or against an
    /// older one it has since moved past.
    [[nodiscard]] virtual std::uint32_t serial() const noexcept = 0;

protected:
    TextInput() = default;
};

/// The zwp_text_input_manager_v3 global (version 1). Move-only; pointer-stable
/// state.
class TextInputManager {
public:
    /// Create the global. Focus is driven by `seat`'s keyboard focus.
    [[nodiscard]] static Result<TextInputManager> create(Display& display, Seat& seat);

    ~TextInputManager();
    TextInputManager(TextInputManager&&) noexcept;
    TextInputManager& operator=(TextInputManager&&) noexcept;
    TextInputManager(const TextInputManager&) = delete;
    TextInputManager& operator=(const TextInputManager&) = delete;

    [[nodiscard]] Signal<NewTextInput>& new_text_input() noexcept;

    /// The text input of the currently focused client that is enabled, or null.
    /// This is the one an input method should be talking to.
    [[nodiscard]] TextInput* focused() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit TextInputManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
