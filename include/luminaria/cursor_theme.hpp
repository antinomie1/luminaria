// luminaria/cursor_theme.hpp — real cursor images, loaded from the user's theme.
//
// `wp_cursor_shape_v1` and `wl_pointer.set_cursor` both end up naming a cursor
// ("default", "text", "ns-resize") rather than handing over pixels. Somebody has
// to turn that name into an image, and on Wayland that somebody is the
// compositor — there is no server-side cursor font.
//
// This reads the XCursor files every desktop already ships in
// /usr/share/icons/<theme>/cursors, including theme inheritance and animated
// cursors. No X11 dependency: the format is a handful of little-endian records
// and parsing it here is smaller than linking libXcursor.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "luminaria/core/expected.hpp"

namespace luminaria {

/// One frame of a cursor. `rgba` is tightly packed, premultiplied RGBA8 —
/// exactly what VulkanRenderer::upload_texture and the DRM cursor plane want.
struct CursorImage {
    int width = 0;
    int height = 0;
    int hotspot_x = 0;
    int hotspot_y = 0;
    std::uint32_t delay_ms = 0; // how long this frame shows; 0 for still cursors
    std::vector<std::uint8_t> rgba;
};

class CursorTheme {
public:
    /// Load a theme. An empty name means $XCURSOR_THEME, then "default".
    /// `size` is the nominal cursor size in pixels; the closest available is
    /// used. Fails only if no theme directory could be found at all.
    [[nodiscard]] static Result<CursorTheme> load(std::string name = {}, int size = 24);

    ~CursorTheme();
    CursorTheme(CursorTheme&&) noexcept;
    CursorTheme& operator=(CursorTheme&&) noexcept;
    CursorTheme(const CursorTheme&) = delete;
    CursorTheme& operator=(const CursorTheme&) = delete;

    /// Every frame of `name`, or null if the theme has no such cursor. Results
    /// are cached, so calling this per frame is fine. Names are the XDG ones —
    /// `cursor_shape_name()` output goes straight in.
    [[nodiscard]] const std::vector<CursorImage>* cursor(const std::string& name);

    /// The frame to show at `time_ms` (CLOCK_MONOTONIC milliseconds). Still
    /// cursors ignore the time; animated ones cycle. Null if unknown.
    [[nodiscard]] const CursorImage* frame(const std::string& name, std::uint32_t time_ms);

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] int size() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit CursorTheme(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
