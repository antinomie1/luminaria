// luminaria — a minimal Wayland compositor library.
//
//     import luminaria;          // core protocols + nested/headless backends
//     import luminaria.gpu;      // Vulkan, DRM/input/session and GPU protocols
//     import luminaria.desktop;  // privileged desktop-shell protocols
//     import luminaria.xwayland; // X11 bridge
//
// This is the dependency-light primary interface. Everything below is one of
// its partitions, one per concept, and each partition file holds both its
// interface and implementation. Consumers name one of the four public modules,
// never a partition.
//
// C libraries stay in each unit's global module fragment, so importing a module
// exports none of their declarations. Code that talks to libwayland directly — a test acting
// as a client, say — includes those headers itself, and gets the same types:
// the opaque forward declarations here live in the global module fragment for
// exactly that reason.
export module luminaria;

export import :backend;
export import :box;
export import :client_buffer;
export import :color;
export import :compositor;
export import :cursor_shape;
export import :cursor_theme;
export import :data_device;
export import :display;
export import :dmabuf;
export import :event_loop;
export import :expected;
export import :fractional_scale;
export import :handle;
export import :headless;
export import :idle_inhibit;
export import :idle_notify;
export import :input_event;
export import :layer_shell;
export import :output;
export import :output_global;
export import :output_layout;
export import :pixel;
export import :pixel_layout;
export import :pointer_constraints;
export import :presentation_time;
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
export import :wayland;
export import :xdg_activation;
export import :xdg_decoration;
export import :xdg_shell;
