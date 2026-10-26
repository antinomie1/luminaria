// luminaria_rs.h — the one plain header the Rust binding stands on.
//
// cxx's generated glue is compiled by the `cc` crate, which knows nothing about
// C++ modules or BMIs, so it has to be able to #include something. This is it:
// no `import`, no luminaria type, nothing but an opaque pimpl. Everything that
// touches the modules lives in luminaria_rs.cpp, which xmake compiles.
//
// The shared structs (Config, Event, Item, ...) are defined by cxx in the
// generated lib.rs.h; they are only forward-declared here, which is all a
// declaration that takes them by reference, slice or return value needs.
#pragma once

#include <cstdint>
#include <memory>

#include "rust/cxx.h"

namespace lum {

struct Config;
struct Event;
struct Item;
struct Mode;
struct Rect;
struct Size;

/// A whole compositor: display, socket, backend, the standard globals and the
/// renderer. Rust drives it — `dispatch()`, then `next_event()` until empty —
/// and C++ never calls back into Rust, so nothing can re-enter it.
class Server {
public:
    ~Server(); // out of line: only the .cpp sees a complete Impl
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    rust::String socket_name() const;
    int fd() const;

    bool dispatch(int timeout_ms);
    bool next_event(Event& out);

    void set_scene(rust::Slice<const Item> items);
    void set_background(float r, float g, float b, float a);

    void focus(std::uint64_t window);
    void configure(std::uint64_t window, std::int32_t width, std::int32_t height);
    void set_maximized(std::uint64_t window, bool on);
    void set_fullscreen(std::uint64_t window, bool on);
    void set_minimized(std::uint64_t window, bool on);
    void set_tiled(std::uint64_t window, bool on);
    void set_resizing(std::uint64_t window, bool on);
    void close(std::uint64_t window);
    rust::String window_title(std::uint64_t window) const;
    rust::String window_app_id(std::uint64_t window) const;
    rust::String window_icon_name(std::uint64_t window) const;
    Size window_size(std::uint64_t window) const;
    Size window_min_size(std::uint64_t window) const;
    std::uint64_t window_parent(std::uint64_t window) const;

    void grab_pointer(bool on);
    std::uint32_t bind_key(std::uint32_t mods, std::uint32_t keysym);
    bool set_keyboard_layout(rust::Str layout);

    Rect output_box(std::uint64_t output) const;
    Rect output_usable(std::uint64_t output) const;
    rust::Vec<Mode> output_modes(std::uint64_t output) const;
    bool set_output_mode(std::uint64_t output, std::int32_t width, std::int32_t height,
                         std::int32_t refresh_mhz, rust::String& error);
    void set_output_scale(std::uint64_t output, std::int32_t scale);
    void set_output_transform(std::uint64_t output, std::uint32_t transform);
    void set_output_position(std::uint64_t output, std::int32_t x, std::int32_t y);

    std::uint32_t add_workspace(rust::Str name);
    void remove_workspace(std::uint32_t workspace);
    void activate_workspace(std::uint32_t workspace);

    bool is_locked() const;

    struct Impl;
    explicit Server(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

/// Null on failure, with the reason in `error`. Never throws: luminaria does
/// not let exceptions cross the C boundary, and cxx's Result mapping is built
/// on exactly that, so it is not used anywhere in this bridge.
std::unique_ptr<Server> server_new(const Config& config, rust::String& error);

/// xkb keysym for a name such as "Return" or "q"; 0 when there is none.
std::uint32_t keysym_from_name(rust::Str name);

} // namespace lum
