// luminaria/protocol/desktop_globals.cppm — standard desktop Wayland globals suite.
module;

#include <wayland-server-core.h>

export module luminaria.desktop:desktop_globals;

import std;

import luminaria;
import luminaria.gpu;

import :data_control;


export namespace luminaria {

/// Owns the standard set of Wayland desktop globals and manages their lifecycle
/// and declaration-order destruction.
class DesktopGlobals {

public:
    [[nodiscard]] static Result<DesktopGlobals>
    create(Display& display, VulkanRenderer* renderer = nullptr, std::string_view seat_name = "seat0");

    ~DesktopGlobals() = default;
    DesktopGlobals(DesktopGlobals&&) noexcept = default;
    DesktopGlobals& operator=(DesktopGlobals&&) noexcept = default;
    DesktopGlobals(const DesktopGlobals&) = delete;
    DesktopGlobals& operator=(const DesktopGlobals&) = delete;

    [[nodiscard]] Compositor& compositor() noexcept { return compositor_; }
    [[nodiscard]] Subcompositor& subcompositor() noexcept { return subcompositor_; }
    [[nodiscard]] Viewporter& viewporter() noexcept { return viewporter_; }
    [[nodiscard]] XdgShell& xdg_shell() noexcept { return xdg_shell_; }
    [[nodiscard]] LayerShell& layer_shell() noexcept { return layer_shell_; }
    [[nodiscard]] CursorShapeManager& cursor_shape() noexcept { return cursor_shape_; }
    [[nodiscard]] XdgDecorationManager& xdg_decoration() noexcept { return xdg_decoration_; }
    [[nodiscard]] XdgActivation& xdg_activation() noexcept { return xdg_activation_; }
    [[nodiscard]] IdleInhibitManager& idle_inhibit() noexcept { return idle_inhibit_; }
    [[nodiscard]] Seat& seat() noexcept { return seat_; }

    [[nodiscard]] PointerConstraints* pointer_constraints() noexcept {
        return constraints_.has_value() ? &*constraints_ : nullptr;
    }
    [[nodiscard]] RelativePointerManager* relative_pointer() noexcept {
        return relative_.has_value() ? &*relative_ : nullptr;
    }
    [[nodiscard]] ScreencopyManager* screencopy() noexcept {
        return screencopy_.has_value() ? &*screencopy_ : nullptr;
    }
    [[nodiscard]] DataControlManager* data_control() noexcept {
        return data_control_.has_value() ? &*data_control_ : nullptr;
    }

    /// Instantiate globals that hold a reference to Seat.
    [[nodiscard]] Status bind_seat_globals(Display& display);

    /// Helper to attach screencopy frame capture callback to an output.
    static void attach_screencopy_frame(ScreencopyManager& sc, OutputGlobal& global,
                                        int width, int height,
                                        std::function<bool(int x, int y, int w, int h,
                                                           std::vector<std::uint8_t>& rgba)> capture_fn);

private:
    DesktopGlobals(Compositor compositor, Subcompositor subcompositor,
                   Viewporter viewporter, XdgShell xdg_shell,
                   LayerShell layer_shell, CursorShapeManager cursor_shape,
                   XdgDecorationManager xdg_decoration,
                   XdgActivation xdg_activation,
                   IdleInhibitManager idle_inhibit, Seat seat,
                   std::optional<LinuxDmabuf> dmabuf,
                   std::optional<BackgroundEffectManager> background_effect,
                   std::optional<ScreencopyManager> screencopy) noexcept
        : compositor_(std::move(compositor)),
          subcompositor_(std::move(subcompositor)),
          viewporter_(std::move(viewporter)),
          xdg_shell_(std::move(xdg_shell)),
          layer_shell_(std::move(layer_shell)),
          cursor_shape_(std::move(cursor_shape)),
          xdg_decoration_(std::move(xdg_decoration)),
          xdg_activation_(std::move(xdg_activation)),
          idle_inhibit_(std::move(idle_inhibit)),
          seat_(std::move(seat)),
          constraints_{},
          relative_{},
          data_device_{},
          primary_selection_{},
          data_control_{},
          dmabuf_(std::move(dmabuf)),
          background_effect_(std::move(background_effect)),
          screencopy_(std::move(screencopy)) {}

    Compositor compositor_;
    Subcompositor subcompositor_;
    Viewporter viewporter_;
    XdgShell xdg_shell_;
    LayerShell layer_shell_;
    CursorShapeManager cursor_shape_;
    XdgDecorationManager xdg_decoration_;
    XdgActivation xdg_activation_;
    IdleInhibitManager idle_inhibit_;
    Seat seat_;
    // Declared after `seat_` so they are destroyed before `seat_`.
    std::optional<PointerConstraints> constraints_{};
    std::optional<RelativePointerManager> relative_{};
    std::optional<DataDeviceManager> data_device_{};
    std::optional<PrimarySelectionManager> primary_selection_{};
    std::optional<DataControlManager> data_control_{};
    std::optional<LinuxDmabuf> dmabuf_{};
    std::optional<BackgroundEffectManager> background_effect_{};
    std::optional<ScreencopyManager> screencopy_{};
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

Result<DesktopGlobals> DesktopGlobals::create(Display& display,
                                              VulkanRenderer* renderer,
                                              std::string_view seat_name) {
    if (Status shm = display.init_shm(); !shm) {
        return std::unexpected(shm.error());
    }

    Result<Compositor> compositor = Compositor::create(display);
    if (!compositor) {
        return std::unexpected(compositor.error());
    }

    Result<Subcompositor> subcompositor = Subcompositor::create(display);
    if (!subcompositor) {
        return std::unexpected(subcompositor.error());
    }

    Result<Viewporter> viewporter = Viewporter::create(display);
    if (!viewporter) {
        return std::unexpected(viewporter.error());
    }

    Result<XdgShell> xdg_shell = XdgShell::create(display);
    if (!xdg_shell) {
        return std::unexpected(xdg_shell.error());
    }

    Result<LayerShell> layer_shell = LayerShell::create(display);
    if (!layer_shell) {
        return std::unexpected(layer_shell.error());
    }

    Result<CursorShapeManager> cursor_shape = CursorShapeManager::create(display);
    if (!cursor_shape) {
        return std::unexpected(cursor_shape.error());
    }

    Result<XdgDecorationManager> xdg_decoration = XdgDecorationManager::create(display);
    if (!xdg_decoration) {
        return std::unexpected(xdg_decoration.error());
    }

    Result<XdgActivation> xdg_activation = XdgActivation::create(display);
    if (!xdg_activation) {
        return std::unexpected(xdg_activation.error());
    }

    Result<IdleInhibitManager> idle_inhibit = IdleInhibitManager::create(display);
    if (!idle_inhibit) {
        return std::unexpected(idle_inhibit.error());
    }

    Result<Seat> seat = Seat::create(display, std::string(seat_name));
    if (!seat) {
        return std::unexpected(seat.error());
    }


    std::optional<LinuxDmabuf> dmabuf;
    std::optional<BackgroundEffectManager> background_effect;
    if (renderer != nullptr) {
        if (Result<LinuxDmabuf> created = LinuxDmabuf::create(display, renderer)) {
            dmabuf.emplace(std::move(*created));
        }
        if (Result<BackgroundEffectManager> bg = BackgroundEffectManager::create(display)) {
            background_effect.emplace(std::move(*bg));
        }
    }

    std::optional<ScreencopyManager> screencopy;
    if (Result<ScreencopyManager> sc = ScreencopyManager::create(display)) {
        screencopy.emplace(std::move(*sc));
    }

    return DesktopGlobals{
        *std::move(compositor), *std::move(subcompositor), *std::move(viewporter),
        *std::move(xdg_shell),  *std::move(layer_shell),   *std::move(cursor_shape),
        *std::move(xdg_decoration), *std::move(xdg_activation), *std::move(idle_inhibit),
        *std::move(seat), std::move(dmabuf), std::move(background_effect), std::move(screencopy)};
}

Status DesktopGlobals::bind_seat_globals(Display& display) {
    Result<PointerConstraints> constraints = PointerConstraints::create(display, seat_);
    if (!constraints) {
        return std::unexpected(constraints.error());
    }
    constraints_.emplace(std::move(*constraints));

    Result<RelativePointerManager> relative = RelativePointerManager::create(display, seat_);
    if (!relative) {
        return std::unexpected(relative.error());
    }
    relative_.emplace(std::move(*relative));

    Result<DataDeviceManager> data_device = DataDeviceManager::create(display, seat_);
    if (!data_device) {
        return std::unexpected(data_device.error());
    }
    data_device_.emplace(std::move(*data_device));

    Result<PrimarySelectionManager> primary_selection =
        PrimarySelectionManager::create(display, seat_);
    if (!primary_selection) {
        return std::unexpected(primary_selection.error());
    }
    primary_selection_.emplace(std::move(*primary_selection));

    Result<DataControlManager> data_control = DataControlManager::create(
        display, *data_device_,
        primary_selection_.has_value() ? &*primary_selection_ : nullptr);
    if (!data_control) {
        return std::unexpected(data_control.error());
    }
    data_control_.emplace(std::move(*data_control));

    return ok();
}



void DesktopGlobals::attach_screencopy_frame(
    ScreencopyManager& sc, OutputGlobal& global, int width, int height,
    std::function<bool(int x, int y, int w, int h, std::vector<std::uint8_t>& rgba)> capture_fn) {
    global.on_bind([&sc, width, height, fn = std::move(capture_fn)](wl_resource* res) {
        sc.add_output(res, width, height, fn);
    });
}

} // namespace luminaria
