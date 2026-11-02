# Luminaria

English | [简体中文](README.zh-CN.md)

A Wayland compositor library written in modern C++23: built on top of `libwayland-server`, rendered with Vulkan, offering capability-layered APIs via C++23 named modules with zero public headers, and featuring comprehensive Rust bindings powered by `cxx`.

## Design Philosophy

Luminaria is a **mechanism-only library** (Mechanism, not policy). It handles the repetitive, error-prone plumbing that every compositor must implement—frame scheduling, damage accounting, occlusion culling, sync fence coordination, hardware cursor planes, and device hotplugging. The decisions that define a compositor's identity and user experience—window management, tiling algorithms, keybindings, and decorations—remain entirely in the hands of the compositor author.

- **Zero Idle Overhead (Demand-Driven)**: An idle desktop consumes **0 GPU submissions, 0 page flips, and 0 CPU wakeups**. The compositor never spins at 60 Hz or polls timers; vblank events and frame callbacks are processed strictly on demand.
- **Memory Safety by Construction**: Every C handle is RAII-wrapped, and signal listeners disconnect automatically on destruction—no manual `wl_list_remove`. Dangling pointers are eliminated using generational `SurfaceId{index, generation}` handles so recycled slots never reference stale clients.
- **Immediate-Mode Scene & Diff-Based Damage**: Instead of maintaining a fragile retained scene graph (ADR 0001), the compositor submits an immediate-mode placement list each frame. Window movement, resizing, mapping, and unmapping damage is deduced automatically by diffing consecutive placement lists—moving a window redraws only its old and new bounds, not the full screen.
- **Pure C++23 Modules & First-Class Rust**: Zero header files or macro leakage, strictly structured as a unidirectional module DAG. First-class Rust bindings powered by `cxx` mirror every capability with modular Cargo feature flags.

## Key Features

### 🚀 Modern Vulkan 1.3 Rendering Pipeline
- **Zero-Copy GPU Path**: Client dmabufs are imported directly as Vulkan textures via `VK_EXT_image_drm_format_modifier`. Composition renders straight into scanout targets and flips via DRM atomic modesetting with zero CPU readbacks or stalls.
- **Vulkan 1.3 Baseline**: Built entirely on dynamic rendering, `synchronization2`, and timeline semaphores—eliminating classic VkRenderPass/Framebuffer allocation overhead.
- **Front-to-Back Occlusion Culling**: Opaque regions accumulate from front to back; fully covered surfaces (such as wallpapers beneath maximized windows) are never sampled by the GPU.
- **Precise Damage Scissoring**: Damaged regions are broken down into disjoint rectangle sets and rendered with per-rect `vkCmdSetScissor`; zero heap allocations per frame in steady state.
- **Direct Scanout & Advanced Effects**: Bypasses composition for fullscreen windows (Direct Scanout). Supports offscreen passes (`OffscreenTarget`), custom SPIR-V fragment shaders, and x-ray background blur.
- **CPU Fallback Compositor**: Integrated `CpuCompositor` for headless environments, CI pipelines, and GPU-less platforms.

### 🖥️ Comprehensive Backends & Hardware Integration
- **Bare-Metal DRM/KMS**: Atomic modesetting, multi-output hotplug via udev netlink, hardware cursor planes (cursor movement causes zero scene redraws), adaptive sync (VRR / FreeSync / G-Sync), and explicit synchronization (`linux-drm-syncobj-v1`).
- **Nested Wayland Backend**: Runs inside an existing Wayland compositor with server/client-side decoration negotiation (CSD/SSD), zero-copy dmabuf presentation to the host, and transparent input forwarding.
- **Libinput Backend**: Built-in xkb keymap state machine, multi-touch tracking, touchpad gestures (swipe/pinch), drawing tablet support (`zwp_tablet_manager_v2`), and virtual input injection.
- **Session Management**: Seat switching via `libseat` ensures non-root execution and safe VT switching by automatically relinquishing and restoring GPU/input control.

### 📦 Extensive Wayland Protocol Ecosystem
- **Core & Desktop Shell**: `wl_compositor` v6, `xdg_wm_base` v5, `zwlr_layer_shell_v1` (panels, docks, wallpapers, notifications, lock screens), `ext_workspace_v1`, `zwlr_foreign_toplevel_management_v1` (taskbars and window lists).
- **Timing & Presentation**: `wp_presentation` (accurate KMS hardware vblank timestamps), `wp_tearing_control_v1` (low-latency async tearing flips), `wp_fifo_v1`, `wp_commit_timing_v1`.
- **HiDPI & Color**: `wp_fractional_scale_v1` (high-precision 120th-unit fractional scaling), `wp_viewporter`, and parametric color management (`wp_color_manager_v1`).
- **Capture & Screencasting**: Output screencasting (`wlr-screencopy` / `ext-image-copy-capture`), isolated cursor capture streams, and **single-window capture** (`ext-foreign-toplevel-image-capture-source-v1`, independently captured from the surface tree even when occluded or minimized, with full tiled dmabuf support).
- **Input Methods & Security**: Complete IME support via `zwp_text_input_v3` and `zwp_input_method_v2`, pointer constraints, relative pointer motions, and fail-closed session locking (`ext_session_lock_v1`).

### 🦀 First-Class Rust Bindings
- **Modular Cargo Features**: Granular feature flags (`backend`, `render`, `shell`, `desktop`, `text`) mapped to the C++ modules—disabled modules link zero unused system dependencies.
- **Idiomatic Rust API**: Unified `Event` stream, strong types with pattern matching, and optional `mint` / `glam` math interoperability.

## Status

Real clients (such as Firefox, konsole, and weston-terminal) can connect and run. For the complete list of supported features, see **[docs/features.md](docs/features.md)**.

## Quick Start

### C++

```sh
xmake f -y --toolchain=clang && xmake -j4
xmake run minimal
```

For dependencies, running tests, and bare-metal DRM execution, see **[docs/building.md](docs/building.md)**.

### Rust

```sh
cargo run --example minimal
# Bare-metal DRM execution: LUMINARIA_BACKEND=drm cargo run --example minimal
```

For Rust bindings instructions, see **[bindings/rust/README.md](bindings/rust/README.md)**; for full API details, see **[bindings/rust/docs/api.md](bindings/rust/docs/api.md)**.

## Documentation

| Document | Description |
|---|---|
| [CONTEXT.md](CONTEXT.md) | Glossary and terminology definitions |
| [docs/architecture.md](docs/architecture.md) | System layering, module architecture, and core lifecycle rules |
| [docs/api.md](docs/api.md) | Complete C++ API and protocol interface reference |
| [docs/features.md](docs/features.md) | Feature matrix with test coverage status |
| [bindings/rust/README.md](bindings/rust/README.md) | Rust bindings overview, feature flags, and quick start |
| [bindings/rust/docs/api.md](bindings/rust/docs/api.md) | Complete Rust API reference |
| [docs/adr/](docs/adr/) | Architectural Decision Records (ADRs) |
| [TODO.md](TODO.md) | Roadmap, pending tasks, and known technical boundaries |
| [AGENTS.md](AGENTS.md) | Development guidelines and conventions for AI agents |

## License

BSD 2-Clause, see [LICENSE](LICENSE).
