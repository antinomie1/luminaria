// luminaria — a minimal Wayland compositor library.
//
//     import luminaria;
//
// is the whole interface. Everything below is a partition, one per concept,
// and each partition file holds both its interface and its implementation —
// there is no header/source split, because a module unit is not a header.
// Consumers never name a partition: they are an implementation detail of how
// this module is split up.
//
// The C libraries this is built on (libwayland-server, Vulkan, libdrm, xcb)
// stay in each unit's global module fragment, so importing luminaria pulls in
// none of them. Code that wants to talk to libwayland directly — a test acting
// as a client, say — includes those headers itself, and gets the same types:
// the opaque forward declarations here live in the global module fragment for
// exactly that reason.
export module luminaria;

export import :backend;
export import :box;
export import :color;
export import :compositor;
export import :cursor_shape;
export import :cursor_theme;
export import :data_control;
export import :data_device;
export import :display;
export import :dmabuf;
export import :drm;
export import :drm_syncobj;
export import :event_loop;
export import :expected;
export import :foreign_toplevel;
export import :fractional_scale;
export import :handle;
export import :headless;
export import :idle_inhibit;
export import :idle_notify;
export import :input_event;
export import :layer_shell;
export import :libinput;
export import :linux_dmabuf;
export import :output;
export import :output_global;
export import :output_layout;
export import :pixel;
export import :pointer_constraints;
export import :presentation_time;
export import :rect_fill;
export import :region;
export import :relative_pointer;
export import :scene;
export import :screencopy;
export import :seat;
export import :session;
export import :signal;
export import :single_pixel_buffer;
export import :subcompositor;
export import :tearing_control;
export import :text_input;
export import :transform;
export import :viewporter;
export import :vulkan;
export import :wayland;
export import :workspace;
export import :xdg_activation;
export import :xdg_decoration;
export import :xdg_shell;
export import :xwayland;
