// luminaria/backend.cppm — abstracts the display/input hardware behind one interface.
//
// A backend produces Outputs (and, from Phase 3, input devices). The compositor
// subscribes to `new_output` before calling start(). Concrete backends: headless,
// wayland-nested, drm+libinput.

module;

export module luminaria:backend;

import :expected;
import :output;
import :signal;

export namespace luminaria {

struct NewOutput {
    Output& output;
};

class Backend {
public:
    Backend() = default;
    virtual ~Backend() = default;
    Backend(Backend&&) = default;
    Backend& operator=(Backend&&) = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    /// Emit `new_output` for existing outputs and begin producing frame events.
    virtual Status start() = 0;

    Signal<NewOutput> new_output;
    // Input does not come through here: LibinputBackend and WaylandBackend
    // expose their own signals, because what they emit differs (relative vs
    // absolute motion) and the compositor routes them differently.
};

} // namespace luminaria
