// luminaria — a minimal Wayland compositor library.
//
//     import luminaria;           // protocols + mechanism kernel: no hardware
//     import luminaria.backend;   // headless, nested, DRM/KMS, libinput, session
//     import luminaria.render;    // Vulkan, CPU compositing, dmabuf, screencopy
//     import luminaria.shell;     // the immediate-mode frame/cursor pipeline (pulls backend+render)
//     import luminaria.desktop;   // privileged desktop protocols (no GPU needed)
//     import luminaria.text;      // glyph rasterization and font metrics (fcft + pixman)
//
// This is the dependency-light primary interface: libwayland-server and
// xkbcommon, nothing else. It holds the mechanism kernel (Display, EventLoop,
// Signal, Result), the value types, the abstract `Backend`/`Output`/`InputEvent`
// that every backend implements, and every protocol whose semantics are plain
// Wayland rather than something a compositor's shell or its display hardware
// decides. Nothing here knows Vulkan, DRM, libinput or libseat.
//
// Each partition is one concept and holds both its interface and its
// implementation. Consumers name one of the public modules, never a partition.
//
// C libraries stay in each unit's global module fragment, so importing a module
// exports none of their declarations. Code that talks to libwayland directly — a test acting
// as a client, say — includes those headers itself, and gets the same types:
// the opaque forward declarations here live in the global module fragment for
// exactly that reason.
export module luminaria;

export import :backend;
export import :background_effect;
export import :blur;
export import :box;
export import :client_buffer;
export import :color;
export import :commit_timing;
export import :compositor;
export import :content_type;
export import :core_globals;
export import :cursor_shape;
export import :data_device;
export import :display;
export import :dmabuf;
export import :event_loop;
export import :expected;
export import :fifo;
export import :fractional_scale;
export import :handle;
export import :idle_inhibit;
export import :idle_notify;
export import :input_event;
export import :input_router;
export import :keymap;
export import :layer_shell;
export import :match;
export import :output;
export import :output_global;
export import :output_layout;
export import :pixel;
export import :pixel_layout;
export import :pointer_constraints;
export import :presentation_time;
export import :protocol_helper;
export import :rect_fill;
export import :region;
export import :relative_pointer;
export import :seat;
export import :signal;
export import :single_pixel_buffer;
export import :subcompositor;
export import :tearing_control;
export import :text_input;
export import :transform;
export import :viewporter;
export import :xdg_activation;
export import :xdg_decoration;
export import :xdg_shell;
export import :xdg_toplevel_icon;
