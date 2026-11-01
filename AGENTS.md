# AGENTS.md

Guidance for AI coding agents working in Luminaria.

## Project Overview

Luminaria is a minimal Wayland compositor library in modern C++23, built on top of `libwayland-server` and rendering with Vulkan-Hpp. Built with xmake.

### Module Hierarchy (ADR 0008)

Pure C++23 named modules with no public headers (`include/` does not exist). Layered strictly downwards:
- `luminaria`: Core mechanism kernel, geometry/math, and protocols.
- `luminaria.backend`: KMS/DRM, libinput, session (libseat), headless, nested.
- `luminaria.render`: Vulkan 1.3 renderer, CPU compositor, buffer protocols (`linux_dmabuf`, `drm_syncobj`, `screencopy`).
- `luminaria.desktop`: Privileged desktop protocols (`workspace`, `foreign_toplevel`, `session_lock`, etc.).
- `luminaria.shell`: Ledgers and facades (`Frame`, `SceneRenderer`, `ScreenManager`, `DefaultInputRouter`, `Cursor`, `AutoBackend`). Re-exports `backend` and `render`.
- `luminaria.text`: Optional fcft + pixman glyph rasterization.

Prose docs: `README.md` (overview), `CONTEXT.md` (terminology), `docs/architecture.md` (architecture & invariants), `docs/features.md` (feature matrix), `docs/api.md` (API reference), `docs/building.md` (build/run), `docs/adr/` (ADRs), `TODO.md` (open work).

## Build / Test / Run

```sh
xmake f -y --toolchain=clang   # Configure (runs wayland-scanner & glslangValidator)
xmake -j4                      # Build library and examples
xmake build -a -j4             # Build everything including tests
xmake test                     # Run test suite
xmake test test_<name>/*       # Run single test
xmake run minimal              # Run example compositor (nested)
LUMINARIA_BACKEND=headless xmake run minimal # Run headless
LUMINARIA_BACKEND=drm xmake run minimal      # Run bare-metal DRM (needs free VT)

# Sanitizers & Vulkan validation
xmake f --sanitize=address --toolchain=clang && xmake build -a -j4
LSAN_OPTIONS=suppressions=tests/lsan.supp xmake test
LUMINARIA_VK_VALIDATION=1 VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true xmake test
```

- Compiler: Clang ≥ 22 (`--toolchain=clang`) or GCC ≥ 16. Fatal warnings enabled.
- Memory limit: Clang module compilation takes 1–2 GB per unit; always cap parallelism (e.g. `-j4`).
- Test suite: Most tests run in-process clients over `socketpair` (no GPU or host compositor required). Tests skip cleanly (exit 77) when requirements (GPU, seat, tty) are absent.

## C++23 Module Conventions

- **File & partition naming**: Files are partitions. `src/util/box.cppm` is `luminaria:box`. Partitions in submodules use `luminaria.<mod>:<name>`. Partition names are flat; consumers never import partitions directly.
- **Interface & implementation co-located**: Each `.cppm` contains declarations in `export namespace luminaria { ... }`, followed by `// --- implementation`, pimpl `struct X::Impl`, and member definitions.
- **Global Module Fragment (GMF)**: All `#include` directives (standard headers, C system headers, `<wayland-server-core.h>`, `src/detail/wayland_fwd.h`) MUST reside in the GMF between `module;` and `export module ...`. No `#include` in module purview (`-Winclude-angled-in-module-purview`).
- **Strict DAG imports**: Partitions must explicitly `import` their dependencies. No cycles allowed.
- **No TU-local exposure**: Internal types named in module-linkage declarations (such as methods of `struct X::Impl`) cannot live in anonymous namespaces (`-WTU-local-entity-exposure`). Lift them to `namespace luminaria` (unexported).
- **Implementation namespace discipline**: Symbols at namespace scope in implementation sections are shared across partitions in the module. Prefix local aliases (`DcMgr`, `DdMgr`, `FtImpl`).
- **No C++ exceptions across C boundary**: Fallible methods return `Result<T>` / `Status` (`fail("...")`, `ok()`).

## Invariants & Rules That Bite

1. **Demand-driven frames**:
   - `Output::schedule_frame()` requests exactly one `frame` event.
   - Committing does NOT implicitly schedule the next frame. An idle screen subscribes to no vblank, performs 0 GPU submissions, and does 0 flips.
2. **Immediate-mode damage diffing**:
   - No retained scene graph (ADR 0001). `Frame` and `SceneItem` lists are built anew each frame.
   - Window movement, resize, open, and close damage is deduced by `Frame::submit()` diffing the current placement list against the previous frame.
   - Compositors only owe wake-up via `Frame::invalidate()` (or `ScreenManager::invalidate()`).
3. **Generational `SurfaceId`**:
   - Never retain `Surface*` across dispatch cycles. Store `SurfaceId` and resolve via `surface_from_id()` immediately before use. Stale IDs resolve to `nullptr`.
4. **Destruction & teardown order**:
   - `Display` is declared first and destroyed last.
   - Client objects can be destroyed inside `wl_display_destroy` after protocol globals are already freed.
   - Globals must track associated client resources and sever back-pointers or call `detach_resource()` in `~Impl`. Destroy handlers must handle `nullptr` safely.
5. **Renderer lifetime**:
   - `VulkanRenderer`'s `Impl` is managed via `shared_ptr`. Resources (`GpuTexture`, `ScanoutTarget`) hold device leases and can outlive the `VulkanRenderer` instance.
   - Code holding raw `VulkanRenderer*` must verify `alive()` before dispatching.
6. **Logical coordinates vs buffer pixels**:
   - Surface coordinates represent logical dimensions. Always use `surface_width()` / `surface_height()` for layout, hit-testing, and subsurface offsets.
   - Buffer pixels (`buffer_width()`, `buffer_scale()`, transforms) are only used during GPU/CPU rasterization.
7. **Frame callbacks on presentation**:
   - `wl_surface.frame` must NEVER be replied to upon commit.
   - Queue frame callbacks until actual presentation, batched via `Frame::send_frame_done(time_ms)` in `Output::present`.
   - When `Frame::submit()` returns `Presented::unchanged`, emit `send_frame_done()` immediately in the `frame` handler to prevent freezing clients.
8. **Defensive memory bounds checking**:
   - Never trust client-supplied buffer metrics (`width`, `height`, `stride`, `offset`).
   - Validate layouts with `layout_fits()` / `layout_length()` before touching pixel buffers.

## Adding a New Protocol

1. Obtain protocol XML (from system `wayland-protocols` or vendor into `protocol/`).
2. Register the protocol in `protocols` or `local_protocols` in `xmake.lua`.
3. Create `src/protocol/<module>/<name>.cppm` following the pimpl + `Result` pattern.
4. Export the partition in the primary interface (`src/luminaria*.cppm`).
5. Add corresponding unit tests in `tests/test_<name>.cpp` (and verify teardown order in `tests/test_teardown_order.cpp` if client resources reference the global).
