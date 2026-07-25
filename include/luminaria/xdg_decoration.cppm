// luminaria/xdg_decoration.cppm — who draws the title bar.
//
// `zxdg_decoration_manager_v1`. A client with a new xdg_toplevel asks whether
// the compositor will decorate it; the compositor answers, and that answer is
// binding — the client draws its own frame only if told to.
//
// Getting this wrong is visible immediately: promise server-side decorations and
// draw none and the window has no title bar at all; stay silent and GTK/Qt
// windows end up with two.

module;

#include <memory>
#include <optional>

export module luminaria:xdg_decoration;

import :core.expected;
import :core.signal;

export namespace luminaria {

class Display;
class Toplevel;

/// Values match `zxdg_toplevel_decoration_v1.mode`.
enum class DecorationMode {
    client_side = 1,
    server_side = 2,
};

/// "This window wants to know who decorates it." Set `mode` to decide; whatever
/// it holds when the signal returns is what the client is told. `preferred` is
/// the client's own request, empty when it just wants our default.
struct DecorationRequest {
    Toplevel* toplevel; // null if the resource could not be resolved
    std::optional<DecorationMode> preferred;
    DecorationMode mode; // in: the default; out: the decision
};

class XdgDecorationManager {
public:
    [[nodiscard]] static Result<XdgDecorationManager> create(Display& display);

    ~XdgDecorationManager();
    XdgDecorationManager(XdgDecorationManager&&) noexcept;
    XdgDecorationManager& operator=(XdgDecorationManager&&) noexcept;
    XdgDecorationManager(const XdgDecorationManager&) = delete;
    XdgDecorationManager& operator=(const XdgDecorationManager&) = delete;

    /// What clients are told when nothing listens to `request()`.
    ///
    /// The default is `client_side`, and deliberately: this library draws no
    /// decorations, so a compositor built on it that hasn't thought about the
    /// question is better off letting toolkits draw their own frame than
    /// leaving every window without one.
    void set_default_mode(DecorationMode mode) noexcept;
    [[nodiscard]] DecorationMode default_mode() const noexcept;

    /// Fires per decoration object, on creation and on every set_mode /
    /// unset_mode. Answer by writing to `DecorationRequest::mode`.
    [[nodiscard]] Signal<DecorationRequest>& request() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit XdgDecorationManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
