# Luminaria

A minimal, from-scratch Wayland compositor **library** in modern C++, built in
the spirit of wlroots. It sits on top of `libwayland-server` (no re-implementing
the wire protocol), renders with Vulkan, and builds with Meson.

Design principles: **friendly API, efficient internals, simple overall.**

> 中文文档见 [README.zh.md](README.zh.md).

## Dependencies (Fedora)

Core:

```sh
sudo dnf install -y \
  gcc-c++ meson ninja-build pkgconf-pkg-config \
  wayland-devel wayland-protocols-devel \
  pixman-devel libxkbcommon-devel \
  vulkan-loader-devel vulkan-headers glslang mesa-vulkan-drivers \
  mesa-libgbm-devel libdrm-devel \
  libinput-devel libseat-devel systemd-devel
```

Xwayland (optional):

```sh
sudo dnf install -y \
  xorg-x11-server-Xwayland libxcb-devel xcb-util-wm-devel
```

## Build & test

```sh
meson setup build
meson test -C build
```

18 tests: 17 pass + 1 skip (`drm` needs a bare VT, so it skips under a desktop).
Most tests drive the real protocol with an **in-process libwayland client**
(socketpair) — no GPU or parent compositor needed. Vulkan tests use a real GPU;
`wayland-nested` connects to a real parent compositor (runs only when
`WAYLAND_DISPLAY` is set).

## Implemented & tested

| Area | What | Tests |
|---|---|---|
| core | `Result<T>`/`Error`, `CUnique` handle RAII, `Signal<Event>` + RAII `Connection`, `Display`, `EventLoop` + `EventSource` | signal, core |
| util | `Box`, `Color`, `Pixel`, `RectFill` (constexpr) | box |
| render | Vulkan-Hpp RAII: clear, rect compositing, client-texture compositing (real GPU read-back) | vulkan, composite, texture |
| present | `Output::commit_frame(pixels)` presents a rendered frame (headless stores it, DRM writes the dumb buffer) | render-output |
| bridge | wl_shm client buffer → RGBA → GPU composite (real client, end to end) | client-texture |
| protocol | `wl_compositor` / `wl_surface` / `wl_subcompositor` | compositor |
| protocol | xdg-shell: full toplevel lifecycle (configure handshake → map) | xdg |
| protocol | `wl_seat` keyboard (xkb keymap) + pointer, focus + event routing | seat |
| scene | scene tree + positioning + hit-testing + flatten-to-renderer | scene |
| backend | abstract `Backend` + `HeadlessBackend` (software frame pump) | headless |
| backend | `WaylandBackend` (nested): open a window in a parent compositor, present via wl_shm | wayland-nested |
| backend | `DrmBackend` (bare-metal KMS): dumb buffers + double-buffered page-flip + vblank pump | drm (needs tty) |
| backend | `LibinputBackend` (bare-metal input): keyboard/pointer event signals | libinput |
| xwayland | launch Xwayland + minimal XWM (xcb connect, root redirect, map/configure) | xwayland |
| example | `tinyluminaria` (nested/headless), `luminaria-drm-demo`, `luminaria-tty` (bare-metal compositor) | tinyluminaria-smoke |

## Running

**Nested** (opens a window inside your existing desktop and exposes its own
`WAYLAND_DISPLAY` for clients to connect to):

```sh
./build/examples/tinyluminaria                 # nested by default; LUMINARIA_BACKEND=headless forces headless
```

**Bare-metal** (switch to a free VT, stop the desktop; needs DRM master + input
ACLs for that VT):

```sh
./build/examples/luminaria-tty                 # auto-detects /dev/dri/card*, or pass e.g. card1
```

`luminaria-tty` is a full bare-metal compositor: DRM output + libinput input +
wl_compositor/xdg-shell/seat. Each frame it composites the mapped client windows
with Vulkan and scans them out to the monitor; keyboard input routes to the
focused window; Esc quits. It prints its `WAYLAND_DISPLAY` — point a client at it
from another VT/ssh:

```sh
WAYLAND_DISPLAY=wayland-1 weston-terminal
```

## Not implemented (extensions / polish, non-blocking)

- **Window management UI**: move/resize/stacking, software cursor, pointer-focus
  hit-testing (keyboard focus currently follows the most recent mapped window).
- **Compositing quality**: textures placed via `copyBufferToImage` — no scaling,
  no alpha blending, no clipping (a window must fit fully on screen); upgrade to a
  textured-quad pipeline when needed.
- **Buffer types**: wl_shm only (ARGB/XRGB8888); dmabuf zero-copy not wired.
- **Session/output**: libseat session management (VT switch/resume), and a
  `wl_output` global (some clients require it).
- **Full XWM**: the XWM maps/configures X windows, but X↔`wl_surface` association
  (`WL_SURFACE_ID`), ICCCM/EWMH, and override-redirect are still to come.

The `Backend`/`Output` abstractions are in place; each of the above slots in the
same way.

## Notes

Built with `-std=c++23` (the newest feature it relies on is `std::expected`;
everything else is C++20/17). It is memory-safe by construction: every C handle
is RAII-wrapped and every signal listener auto-disconnects, so there is no manual
`wl_list_remove`.
