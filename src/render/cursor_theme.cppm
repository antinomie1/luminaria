// luminaria/cursor_theme.cppm — real cursor images, loaded from the user's theme.
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

module;

#include <sys/stat.h>

export module luminaria:cursor_theme;

import std;

import :expected;

export namespace luminaria {

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

// --------------------------------------------------------------- implementation
namespace luminaria {

struct CursorTheme::Impl {
    std::string name;
    int size = 24;
    std::vector<std::string> dirs; // theme directories, in inheritance order
    std::map<std::string, std::vector<CursorImage>> cache;
};

// Lifted out of the anonymous namespace: `std::vector<RawImage>` instantiates
// allocator comparisons at module scope, and a TU-local element type in those
// is ill-formed under clang. Still unexported — private to module luminaria.
struct RawImage {
    std::uint32_t nominal_size = 0;
    CursorImage image;
};

/// One table-of-contents entry of an XCursor file.
struct XcursorToc {
    std::uint32_t type, subtype, position;
};

namespace {

// ---- the XCursor file format ------------------------------------------------
//
// A tiny archive: a header, a table of contents, and one chunk per image. All
// integers are little-endian. Everything we need is in ~60 lines, which is why
// this is here rather than behind a dependency on libXcursor (and, with it, X11).

constexpr std::uint32_t kMagic = 0x72756358;      // "Xcur"
constexpr std::uint32_t kImageType = 0xfffd0002;  // chunk type for an image
constexpr std::uint32_t kMaxDimension = 0x7fff;

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;

    [[nodiscard]] bool seek(std::size_t at) noexcept {
        if (at > size) {
            return false;
        }
        pos = at;
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& out) noexcept {
        if (pos + 4 > size) {
            return false;
        }
        out = static_cast<std::uint32_t>(data[pos]) |
              (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
              (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
              (static_cast<std::uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return true;
    }
};

/// Every image chunk in the file, at every size the theme provides.
std::vector<RawImage> parse_xcursor(const std::vector<std::uint8_t>& bytes) {
    std::vector<RawImage> out;
    Reader r{bytes.data(), bytes.size()};
    std::uint32_t magic = 0, header = 0, version = 0, ntoc = 0;
    if (!r.u32(magic) || magic != kMagic || !r.u32(header) || !r.u32(version) || !r.u32(ntoc)) {
        return out;
    }
    // The header length is authoritative: a future version may make it longer.
    if (!r.seek(header)) {
        return out;
    }
    std::vector<XcursorToc> toc;
    toc.reserve(ntoc);
    for (std::uint32_t i = 0; i < ntoc; ++i) {
        XcursorToc entry{};
        if (!r.u32(entry.type) || !r.u32(entry.subtype) || !r.u32(entry.position)) {
            return out;
        }
        toc.push_back(entry);
    }

    for (const XcursorToc& entry : toc) {
        if (entry.type != kImageType || !r.seek(entry.position)) {
            continue;
        }
        std::uint32_t chunk_header = 0, chunk_type = 0, subtype = 0, chunk_version = 0;
        std::uint32_t width = 0, height = 0, xhot = 0, yhot = 0, delay = 0;
        if (!r.u32(chunk_header) || !r.u32(chunk_type) || !r.u32(subtype) ||
            !r.u32(chunk_version) || !r.u32(width) || !r.u32(height) || !r.u32(xhot) ||
            !r.u32(yhot) || !r.u32(delay)) {
            continue;
        }
        if (chunk_type != kImageType || width == 0 || height == 0 || width > kMaxDimension ||
            height > kMaxDimension || xhot > width || yhot > height) {
            continue;
        }
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        if (r.pos + pixels * 4 > r.size) {
            continue;
        }
        RawImage raw;
        raw.nominal_size = subtype;
        raw.image.width = static_cast<int>(width);
        raw.image.height = static_cast<int>(height);
        raw.image.hotspot_x = static_cast<int>(xhot);
        raw.image.hotspot_y = static_cast<int>(yhot);
        raw.image.delay_ms = delay;
        raw.image.rgba.resize(pixels * 4);
        for (std::size_t i = 0; i < pixels; ++i) {
            std::uint32_t argb = 0;
            (void)r.u32(argb); // bounds already checked
            // Stored ARGB premultiplied; we hand out RGBA, still premultiplied.
            raw.image.rgba[i * 4 + 0] = static_cast<std::uint8_t>((argb >> 16) & 0xff);
            raw.image.rgba[i * 4 + 1] = static_cast<std::uint8_t>((argb >> 8) & 0xff);
            raw.image.rgba[i * 4 + 2] = static_cast<std::uint8_t>(argb & 0xff);
            raw.image.rgba[i * 4 + 3] = static_cast<std::uint8_t>((argb >> 24) & 0xff);
        }
        out.push_back(std::move(raw));
    }
    return out;
}

// ---- finding themes on disk -------------------------------------------------

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    const auto pos = f.tellg();
    if (pos == std::streampos(-1)) {
        return false;
    }
    const std::streamsize size = pos;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || f.read(reinterpret_cast<char*>(out.data()), size).gcount() == size;
}

bool is_directory(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// Where themes live. $XCURSOR_PATH wins outright, as the spec says; otherwise
/// the usual XDG places plus the legacy ones themes are still installed into.
std::vector<std::string> search_path() {
    std::vector<std::string> dirs;
    auto split_into = [&dirs](const char* list) {
        const char* start = list;
        while (*start != '\0') {
            const char* end = std::strchr(start, ':');
            const std::string dir = end != nullptr ? std::string(start, end) : std::string(start);
            if (!dir.empty()) {
                dirs.push_back(dir);
            }
            if (end == nullptr) {
                break;
            }
            start = end + 1;
        }
    };
    if (const char* env = std::getenv("XCURSOR_PATH"); env != nullptr) {
        split_into(env);
        return dirs;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        dirs.push_back(std::string(home) + "/.local/share/icons");
        dirs.push_back(std::string(home) + "/.icons");
    }
    if (const char* xdg = std::getenv("XDG_DATA_DIRS"); xdg != nullptr) {
        std::vector<std::string> base;
        std::swap(base, dirs);
        split_into(xdg);
        for (std::string& d : dirs) {
            d += "/icons";
        }
        dirs.insert(dirs.begin(), base.begin(), base.end());
    }
    dirs.push_back("/usr/local/share/icons");
    dirs.push_back("/usr/share/icons");
    dirs.push_back("/usr/share/pixmaps");
    return dirs;
}

/// Themes that `theme` inherits from, read out of its index.theme.
std::vector<std::string> inherits_of(const std::string& theme_dir) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(theme_dir + "/index.theme", bytes)) {
        return {};
    }
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::vector<std::string> parents;
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        std::size_t line_end = text.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = text.size();
        }
        const std::string line = text.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        constexpr const char* kKey = "Inherits";
        if (line.compare(0, std::strlen(kKey), kKey) != 0) {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        // Values are separated by ';' or ',' depending on who wrote the file.
        std::string value = line.substr(eq + 1);
        std::string current;
        for (const char c : value) {
            if (c == ';' || c == ',' || c == '\r') {
                if (!current.empty()) {
                    parents.push_back(current);
                }
                current.clear();
            } else if (c != ' ' || !current.empty()) {
                current += c;
            }
        }
        while (!current.empty() && current.back() == ' ') {
            current.pop_back();
        }
        if (!current.empty()) {
            parents.push_back(current);
        }
        break;
    }
    return parents;
}

/// Every `<dir>/cursors` to try, in order: the theme itself, then what it
/// inherits, then "default" and Adwaita as a backstop. Depth-limited so a theme
/// that inherits itself cannot loop.
void collect_theme_dirs(const std::string& theme, const std::vector<std::string>& roots,
                        std::vector<std::string>& out, std::vector<std::string>& seen, int depth) {
    if (theme.empty() || depth > 8 ||
        std::find(seen.begin(), seen.end(), theme) != seen.end()) {
        return;
    }
    seen.push_back(theme);
    std::vector<std::string> parents;
    for (const std::string& root : roots) {
        const std::string theme_dir = root + "/" + theme;
        if (!is_directory(theme_dir)) {
            continue;
        }
        if (const std::string cursors = theme_dir + "/cursors"; is_directory(cursors)) {
            out.push_back(cursors);
        }
        for (const std::string& parent : inherits_of(theme_dir)) {
            if (std::find(parents.begin(), parents.end(), parent) == parents.end()) {
                parents.push_back(parent);
            }
        }
    }
    for (const std::string& parent : parents) {
        collect_theme_dirs(parent, roots, out, seen, depth + 1);
    }
}

} // namespace

CursorTheme::CursorTheme(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CursorTheme::~CursorTheme() = default;
CursorTheme::CursorTheme(CursorTheme&&) noexcept = default;
CursorTheme& CursorTheme::operator=(CursorTheme&&) noexcept = default;

Result<CursorTheme> CursorTheme::load(std::string name, int size) {
    if (name.empty()) {
        if (const char* env = std::getenv("XCURSOR_THEME"); env != nullptr && *env != '\0') {
            name = env;
        } else {
            name = "default";
        }
    }
    if (size <= 0) {
        if (const char* env = std::getenv("XCURSOR_SIZE"); env != nullptr) {
            size = std::atoi(env);
        }
        if (size <= 0) {
            size = 24;
        }
    }

    auto impl = std::make_unique<Impl>();
    impl->name = name;
    impl->size = size;
    const std::vector<std::string> roots = search_path();
    std::vector<std::string> seen;
    collect_theme_dirs(name, roots, impl->dirs, seen, 0);
    // Themes routinely omit cursors and rely on inheritance that isn't declared;
    // these two are what everything else falls back to in practice.
    collect_theme_dirs("default", roots, impl->dirs, seen, 0);
    collect_theme_dirs("Adwaita", roots, impl->dirs, seen, 0);
    if (impl->dirs.empty()) {
        return fail("cursor theme '" + name + "' not found (no cursors directory)");
    }
    return CursorTheme{std::move(impl)};
}

const std::vector<CursorImage>* CursorTheme::cursor(const std::string& name) {
    if (auto it = impl_->cache.find(name); it != impl_->cache.end()) {
        return it->second.empty() ? nullptr : &it->second;
    }
    std::vector<CursorImage> frames;
    for (const std::string& dir : impl_->dirs) {
        std::vector<std::uint8_t> bytes;
        // Aliases inside a theme are symlinks, so opening by name is enough.
        if (!read_file(dir + "/" + name, bytes)) {
            continue;
        }
        std::vector<RawImage> images = parse_xcursor(bytes);
        if (images.empty()) {
            continue;
        }
        // Pick the nominal size closest to what was asked for, then keep every
        // frame at that size — that set is the animation.
        std::uint32_t best = images.front().nominal_size;
        for (const RawImage& raw : images) {
            const auto want = static_cast<std::uint32_t>(impl_->size);
            const auto diff = [want](std::uint32_t s) { return s > want ? s - want : want - s; };
            if (diff(raw.nominal_size) < diff(best)) {
                best = raw.nominal_size;
            }
        }
        for (RawImage& raw : images) {
            if (raw.nominal_size == best) {
                frames.push_back(std::move(raw.image));
            }
        }
        break; // first theme in the inheritance chain that has it wins
    }
    auto [it, _] = impl_->cache.emplace(name, std::move(frames));
    return it->second.empty() ? nullptr : &it->second;
}

const CursorImage* CursorTheme::frame(const std::string& name, std::uint32_t time_ms) {
    const std::vector<CursorImage>* frames = cursor(name);
    if (frames == nullptr) {
        return nullptr;
    }
    if (frames->size() == 1) {
        return &frames->front();
    }
    std::uint32_t total = 0;
    for (const CursorImage& f : *frames) {
        total += f.delay_ms;
    }
    if (total == 0) {
        return &frames->front(); // frames with no delays: not really animated
    }
    std::uint32_t at = time_ms % total;
    for (const CursorImage& f : *frames) {
        if (at < f.delay_ms) {
            return &f;
        }
        at -= f.delay_ms;
    }
    return &frames->back();
}

const std::string& CursorTheme::name() const noexcept { return impl_->name; }
int CursorTheme::size() const noexcept { return impl_->size; }

} // namespace luminaria
