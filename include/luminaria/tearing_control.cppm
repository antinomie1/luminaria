// luminaria/tearing_control.cppm — the wp_tearing_control_manager_v1 global.
//
// A client (a game, mostly) says "show my frame the instant it is ready, don't
// wait for vblank". The hint lands on the Surface (`Surface::tearing_hint()`)
// and the compositor decides: it only makes sense for a surface that owns the
// whole output, since an async flip updates the entire scanout buffer.
//
// Importing luminaria pulls in no libwayland headers: the C types this
// interface names are forward-declared in the global module fragment.

module;

#include <memory>

export module luminaria:tearing_control;

import :core.expected;

export namespace luminaria {

class Display;

/// The wp_tearing_control_manager_v1 global (version 1). Move-only;
/// pointer-stable state so the libwayland global can hold a pointer to it.
class TearingControlManager {
public:
    [[nodiscard]] static Result<TearingControlManager> create(Display& display);

    ~TearingControlManager();
    TearingControlManager(TearingControlManager&&) noexcept;
    TearingControlManager& operator=(TearingControlManager&&) noexcept;
    TearingControlManager(const TearingControlManager&) = delete;
    TearingControlManager& operator=(const TearingControlManager&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    explicit TearingControlManager(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace luminaria
