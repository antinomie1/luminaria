// luminaria/protocol/core_globals.cppm — the globals every compositor advertises.
//
// A compositor that clients can actually use needs the same dozen globals no
// matter what its policy is: a toolkit window will not appear without
// xdg-shell, will not paste without a data device, and scales badly without
// viewporter and fractional-scale. Creating them one by one, each with its own
// error check, in an order that has to respect who borrows whom (the data
// devices borrow the seat), was the first sixty lines of every compositor.
//
// Everything else stays opt-in and is created next to this the same way it
// always was: linux-dmabuf (it needs the renderer), the privileged desktop
// protocols (ADR 0003), and anything a policy wants to say no to.

module;

#include <typeinfo>

export module luminaria:core_globals;

import std;

import :compositor;
import :cursor_shape;
import :data_device;
import :display;
import :expected;
import :fractional_scale;
import :presentation_time;
import :seat;
import :single_pixel_buffer;
import :subcompositor;
import :viewporter;
import :xdg_decoration;
import :xdg_shell;

export namespace luminaria {

/// wl_shm, wl_compositor, wl_subcompositor, xdg-shell, wl_seat, the clipboard
/// and primary selection, single-pixel buffers, presentation-time, viewporter,
/// fractional-scale, cursor-shape and xdg-decoration — created together and
/// destroyed in an order that is always safe. Move-only; the globals keep their
/// addresses across a move, so references handed out stay valid.
///
/// Like every global it must die before the `Display`, and — as the frame
/// callbacks it releases are answered from output events — be declared before
/// the backends, so the `Presentation` outlives an output's last `present`.
class CoreGlobals {
public:
    [[nodiscard]] static Result<CoreGlobals> create(Display& display,
                                                    std::string seat_name = "seat0");

    ~CoreGlobals();
    CoreGlobals(CoreGlobals&&) noexcept;
    CoreGlobals& operator=(CoreGlobals&&) noexcept;
    CoreGlobals(const CoreGlobals&) = delete;
    CoreGlobals& operator=(const CoreGlobals&) = delete;

    [[nodiscard]] Compositor& compositor() noexcept;
    [[nodiscard]] Subcompositor& subcompositor() noexcept;
    [[nodiscard]] XdgShell& shell() noexcept;
    [[nodiscard]] Seat& seat() noexcept;
    [[nodiscard]] DataDeviceManager& data_device() noexcept;
    [[nodiscard]] PrimarySelectionManager& primary_selection() noexcept;
    [[nodiscard]] SinglePixelBufferManager& single_pixel_buffer() noexcept;
    [[nodiscard]] Presentation& presentation() noexcept;
    [[nodiscard]] Viewporter& viewporter() noexcept;
    [[nodiscard]] FractionalScaleManager& fractional_scale() noexcept;
    [[nodiscard]] CursorShapeManager& cursor_shape() noexcept;
    /// Answers client-side by default: luminaria draws no title bars.
    [[nodiscard]] XdgDecorationManager& decoration() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit CoreGlobals(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria

// --------------------------------------------------------------- implementation
namespace luminaria {

// Declaration order is destruction order, reversed: the selection managers
// borrow the seat, so they come after it and die before it.
struct CoreGlobals::Impl {
    Compositor compositor;
    Subcompositor subcompositor;
    XdgShell shell;
    Seat seat;
    SinglePixelBufferManager single_pixel_buffer;
    Presentation presentation;
    Viewporter viewporter;
    FractionalScaleManager fractional_scale;
    CursorShapeManager cursor_shape;
    XdgDecorationManager decoration;
    // Created once the seat is at its final address: both keep a `Seat*`.
    std::optional<DataDeviceManager> data_device;
    std::optional<PrimarySelectionManager> primary_selection;
};

namespace {

Result<std::unique_ptr<CoreGlobals::Impl>> make_core_globals(Display& display,
                                                             std::string seat_name) {
    if (Status shm = display.init_shm(); !shm) {
        return std::unexpected(shm.error());
    }
    auto compositor = Compositor::create(display);
    auto subcompositor = Subcompositor::create(display);
    auto shell = XdgShell::create(display);
    auto seat = Seat::create(display, std::move(seat_name));
    auto single_pixel = SinglePixelBufferManager::create(display);
    auto presentation = Presentation::create(display);
    auto viewporter = Viewporter::create(display);
    auto fractional = FractionalScaleManager::create(display);
    auto cursor_shape = CursorShapeManager::create(display);
    auto decoration = XdgDecorationManager::create(display);
    for (const Error* error :
         {compositor ? nullptr : &compositor.error(), subcompositor ? nullptr : &subcompositor.error(),
          shell ? nullptr : &shell.error(), seat ? nullptr : &seat.error(),
          single_pixel ? nullptr : &single_pixel.error(),
          presentation ? nullptr : &presentation.error(),
          viewporter ? nullptr : &viewporter.error(), fractional ? nullptr : &fractional.error(),
          cursor_shape ? nullptr : &cursor_shape.error(),
          decoration ? nullptr : &decoration.error()}) {
        if (error != nullptr) {
            return std::unexpected(*error);
        }
    }
    auto impl = std::make_unique<CoreGlobals::Impl>(CoreGlobals::Impl{
        std::move(*compositor), std::move(*subcompositor), std::move(*shell), std::move(*seat),
        std::move(*single_pixel), std::move(*presentation), std::move(*viewporter),
        std::move(*fractional), std::move(*cursor_shape), std::move(*decoration), {}, {}});
    auto data_device = DataDeviceManager::create(display, impl->seat);
    if (!data_device) {
        return std::unexpected(data_device.error());
    }
    impl->data_device.emplace(std::move(*data_device));
    auto primary = PrimarySelectionManager::create(display, impl->seat);
    if (!primary) {
        return std::unexpected(primary.error());
    }
    impl->primary_selection.emplace(std::move(*primary));
    return impl;
}

} // namespace

Result<CoreGlobals> CoreGlobals::create(Display& display, std::string seat_name) {
    auto impl = make_core_globals(display, std::move(seat_name));
    if (!impl) {
        return std::unexpected(impl.error());
    }
    return CoreGlobals{std::move(*impl)};
}

CoreGlobals::CoreGlobals(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CoreGlobals::~CoreGlobals() = default;
CoreGlobals::CoreGlobals(CoreGlobals&&) noexcept = default;
CoreGlobals& CoreGlobals::operator=(CoreGlobals&&) noexcept = default;

Compositor& CoreGlobals::compositor() noexcept { return impl_->compositor; }
Subcompositor& CoreGlobals::subcompositor() noexcept { return impl_->subcompositor; }
XdgShell& CoreGlobals::shell() noexcept { return impl_->shell; }
Seat& CoreGlobals::seat() noexcept { return impl_->seat; }
DataDeviceManager& CoreGlobals::data_device() noexcept { return *impl_->data_device; }
PrimarySelectionManager& CoreGlobals::primary_selection() noexcept {
    return *impl_->primary_selection;
}
SinglePixelBufferManager& CoreGlobals::single_pixel_buffer() noexcept {
    return impl_->single_pixel_buffer;
}
Presentation& CoreGlobals::presentation() noexcept { return impl_->presentation; }
Viewporter& CoreGlobals::viewporter() noexcept { return impl_->viewporter; }
FractionalScaleManager& CoreGlobals::fractional_scale() noexcept {
    return impl_->fractional_scale;
}
CursorShapeManager& CoreGlobals::cursor_shape() noexcept { return impl_->cursor_shape; }
XdgDecorationManager& CoreGlobals::decoration() noexcept { return impl_->decoration; }

} // namespace luminaria
