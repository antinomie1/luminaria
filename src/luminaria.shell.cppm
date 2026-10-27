// The opinionated half: every ledger each compositor would otherwise reimplement.
//
//     import luminaria.shell;
//
// `Frame` (the per-output frame ledger: placements, damage diffing, fence
// threading, direct scanout), `SceneItem` + `SceneRenderer` (one ordered
// primitive list drawn by whichever of GPU and CPU this machine has, and
// hit-tested against the same list), `Cursor`, `CursorManager`, `LayerSurface`,
// `PopupTree`, `DirectScanout`, and `AutoBackend` — the pick between nested,
// bare metal and headless, which is a policy decision and therefore lives above
// the two modules it chooses between.
//
// Still immediate-mode: nothing here survives a frame except memory (ADR 0001).
// This module re-exports `luminaria.backend` and `luminaria.render`, so a
// compositor built on it names one module and gets all three.
export module luminaria.shell;

export import luminaria;
export import luminaria.backend;
export import luminaria.render;

export import :auto_backend;
export import :cursor;
export import :cursor_manager;
export import :cursor_theme;
export import :direct_scanout;
export import :frame;
export import :layer;
export import :pointer;
export import :popup;
export import :scene;
export import :scene_renderer;
