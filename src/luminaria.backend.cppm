// Everything that puts pixels on a display or keys in a seat.
//
//     import luminaria.backend;
//
// The outputs and the input devices, all of them: the headless frame pump the
// CI uses, the nested window inside a parent compositor, KMS atomic modesetting
// on bare metal, libinput, and the libseat session that decides who may touch
// them. The abstract side — `Backend`, `Output`, `InputEvent`, `OutputLayout` —
// is in the core module, because protocols talk to an output without caring
// which driver is behind it; this module is the implementations.
//
// Deliberately independent of rendering (ADR 0008): nothing here imports
// Vulkan, and a CPU-only compositor on DRM links libdrm/libinput/libseat and no
// GPU stack at all.
export module luminaria.backend;

export import luminaria;
export import :drm;
export import :headless;
export import :libinput;
export import :session;
export import :wayland;
