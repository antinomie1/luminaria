// trace — the evidence scripts/tty-check.sh reads out of a run of minimal,
// kept out of minimal itself. LUMINARIA_TRACE=1 turns it on.
//
// Everything here only listens: the backend, the layout, the shell and the
// renderer's own frame counts. An idle screen asks for no frames at all, so
// the `frames/s` line stops appearing — which is the point of printing it.
export module minimal_trace;

import luminaria.shell;
import std;

export namespace trace {

namespace lm = luminaria;

class Trace {
public:
    Trace(lm::Display& display, lm::AutoBackend& backend, lm::SceneRenderer& renderer,
          lm::OutputLayout& layout, lm::XdgShell& shell) {
        const lm::BackendKind kind = backend.kind();
        const bool on_drm = kind == lm::BackendKind::drm;
        if (backend.session() != nullptr) {
            std::println("minimal: session = libseat (VT switching enabled)");
            connections_ += backend.session()->activity().connect([](lm::SessionActive& e) {
                std::println("minimal: session {}",
                             e.active ? "active — VT switched in" : "inactive — VT switched away");
            });
        } else if (on_drm) {
            std::println("minimal: session = none ({}); VT switching disabled",
                         backend.session_error());
        }
        std::println("minimal: backend = {}", lm::backend_kind_name(kind));
        if (renderer.gpu_enabled()) {
            std::println("minimal: renderer = {}", renderer.gpu()->device_name());
        } else {
            std::println("minimal: renderer = CPU ({})", renderer.gpu_error());
        }

        connections_ += backend.backend().new_output.connect([this, &layout](lm::NewOutput& e) {
            lm::Output& output = e.output;
            const lm::Box box = layout.box_of(output);
            std::println("minimal: output {}x{}@{:.3g}Hz at ({},{}), scale {}, cursor plane {}, vrr {}",
                         output.width(), output.height(), output.refresh_hz(), box.x, box.y,
                         output.scale(), output.has_cursor_plane() ? "yes" : "no",
                         output.vrr_enabled() ? "on" : output.vrr_capable() ? "off" : "n/a");
            connections_ += output.destroy.connect([] { std::println("minimal: output removed"); });
        });
        if (lm::InputSource* source = backend.input(); source != nullptr) {
            connections_ += source->input().connect([](lm::InputEvent& event) {
                if (const auto* caps = std::get_if<lm::InputCapabilities>(&event)) {
                    std::println("minimal: input devices: keyboard={} pointer={}",
                                 caps->keyboard ? "yes" : "no", caps->pointer ? "yes" : "no");
                }
            });
        }
        connections_ += shell.new_toplevel().connect([this](lm::NewToplevel& e) {
            lm::Toplevel* t = &e.toplevel;
            connections_ += t->map.connect(
                [t] { std::println("minimal: window mapped — \"{}\"", t->title()); });
        });

        if (on_drm) {
            report_ = display.event_loop().add_timer([this, &renderer] {
                const lm::SceneStats now = renderer.stats();
                if (!(now == reported_)) {
                    std::println("minimal: frames/s composited={} scanout={} unchanged={} cpu={}",
                                 now.composited - reported_.composited,
                                 now.scanout - reported_.scanout,
                                 now.unchanged - reported_.unchanged, now.cpu - reported_.cpu);
                    reported_ = now;
                }
                report_.arm(1000);
            });
            report_.arm(1000);
        }
    }

private:
    lm::Connections connections_;
    lm::EventSource report_;
    lm::SceneStats reported_;
};

} // namespace trace
