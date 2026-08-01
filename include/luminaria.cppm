// luminaria — a minimal Wayland compositor library.
//
//     import luminaria;
//
// is the whole interface. Everything below is an interface partition, one per
// former public header, and the file layout under include/luminaria/ still
// mirrors src/. Consumers never name a partition: they are an implementation
// detail of how this module is split up.
//
// The C libraries this is built on (libwayland-server, Vulkan, libdrm, xcb)
// stay in each unit's global module fragment, so importing luminaria pulls in
// none of them. Code that wants to talk to libwayland directly — a test acting
// as a client, say — includes those headers itself, and gets the same types:
// the opaque forward declarations here live in the global module fragment for
// exactly that reason.
export module luminaria;

export import :backend;
export import :backend.drm;
export import :backend.headless;
export import :backend.libinput;
export import :backend.wayland;
export import :compositor;
export import :core.display;
export import :core.event_loop;
export import :core.expected;
export import :core.handle;
export import :core.signal;
export import :cursor_shape;
export import :cursor_theme;
export import :data_device;
export import :drm_syncobj;
export import :foreign_toplevel;
export import :fractional_scale;
export import :input_event;
export import :layer_shell;
export import :linux_dmabuf;
export import :output;
export import :output_global;
export import :output_layout;
export import :presentation_time;
export import :render.vulkan;
export import :pointer_constraints;
export import :scene;
export import :screencopy;
export import :relative_pointer;
export import :seat;
export import :session;
export import :single_pixel_buffer;
export import :subcompositor;
export import :tearing_control;
export import :util.box;
export import :util.color;
export import :util.dmabuf;
export import :util.pixel;
export import :util.rect_fill;
export import :util.region;
export import :util.transform;
export import :viewporter;
export import :workspace;
export import :xdg_activation;
export import :xdg_decoration;
export import :xdg_shell;
export import :xwayland;
