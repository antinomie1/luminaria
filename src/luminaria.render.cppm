// Turning client buffers into output pixels, by GPU or by CPU.
//
//     import luminaria.render;
//
// The Vulkan 1.3 renderer (dynamic rendering, dmabuf import with no copy, a
// scanout target exported back as a dmabuf, explicit sync), the CPU compositor
// that stands in when there is no device, and the three protocols that only
// exist to move buffers around: linux-dmabuf, drm-syncobj and screencopy.
//
// Independent of `luminaria.backend` (ADR 0008): the renderer takes logical
// coordinates and a `ScanoutTarget` and never asks which driver is presenting
// them. That is what lets a nested or headless compositor composite on the GPU
// and read the result back, and what keeps `Frame` — which needs both halves —
// in the shell module above them.
export module luminaria.render;

export import luminaria;
export import :cpu_compositor;
export import :drm_syncobj;
export import :linux_dmabuf;
export import :screencopy;
export import :vulkan;
