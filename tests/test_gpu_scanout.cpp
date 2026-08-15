// The GPU compositing chain with no CPU read-back in it: upload a texture,
// composite it into a scanout target that is itself a dmabuf, then read that
// dmabuf back through Vulkan (what KMS would scan out instead). Skips (77)
// without a Vulkan device or without dmabuf support.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <drm_fourcc.h>

import luminaria;

int main() {
    auto renderer = luminaria::VulkanRenderer::create();
    if (!renderer) {
        std::fprintf(stderr, "skip: %s\n", renderer.error().message.c_str());
        return 77;
    }
    if (!renderer->dmabuf_supported()) {
        std::fprintf(stderr, "skip: GPU has no dmabuf import/export\n");
        return 77;
    }

    constexpr int kW = 8, kH = 8;
    // 2x2 solid red, uploaded once and then scaled up by the blit.
    std::vector<std::uint8_t> red(2 * 2 * 4);
    for (size_t i = 0; i < 4; ++i) {
        red[i * 4 + 0] = 255;
        red[i * 4 + 3] = 255;
    }
    auto texture = renderer->upload_texture(2, 2, red);
    assert(texture.has_value());

    const auto mods = renderer->scanout_modifiers(DRM_FORMAT_XRGB8888);
    auto target = renderer->create_scanout(kW, kH, DRM_FORMAT_XRGB8888, mods);
    if (!target) {
        std::fprintf(stderr, "skip: %s\n", target.error().message.c_str());
        return 77;
    }
    assert(target->width() == kW && target->height() == kH);
    assert(target->plane().fd >= 0);
    assert(target->plane().stride >= kW * 4);

    const luminaria::GpuTextureFill fill{&*texture, 0, 0, 4, 4};
    auto status = renderer->render_to(*target, luminaria::Color{0, 0, 1, 1}, {}, {&fill, 1});
    assert(status.has_value());

    // Read the scanout dmabuf back the way an external consumer would.
    const luminaria::DmabufPlane& p = target->plane();
    auto pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                          p.modifier);
    assert(pixels.has_value());
    auto at = [&](int x, int y) {
        const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
        return std::vector<std::uint8_t>{(*pixels)[i], (*pixels)[i + 1], (*pixels)[i + 2]};
    };
    assert((at(1, 1) == std::vector<std::uint8_t>{255, 0, 0})); // texture
    assert((at(6, 6) == std::vector<std::uint8_t>{0, 0, 255})); // background

    // Rotated output: the same logical top-left quad must land top-RIGHT on a
    // framebuffer rotated 90 degrees.
    status = renderer->render_to(*target, luminaria::Color{0, 0, 1, 1}, {}, {&fill, 1}, {},
                                 {luminaria::Transform::rotate_90, 1});
    assert(status.has_value());
    pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                     p.modifier);
    assert(pixels.has_value());
    assert((at(5, 1) == std::vector<std::uint8_t>{255, 0, 0})); // texture, now top-right
    assert((at(1, 1) == std::vector<std::uint8_t>{0, 0, 255})); // background, top-left

    // Scale 2: the logical space is 4x4, so a 2x2 logical quad covers 4x4 pixels.
    const luminaria::GpuTextureFill small{&*texture, 0, 0, 2, 2};
    status = renderer->render_to(*target, luminaria::Color{0, 0, 1, 1}, {}, {&small, 1}, {},
                                 {luminaria::Transform::normal, 2});
    assert(status.has_value());
    pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                     p.modifier);
    assert(pixels.has_value());
    assert((at(3, 3) == std::vector<std::uint8_t>{255, 0, 0})); // inside the scaled quad
    assert((at(5, 5) == std::vector<std::uint8_t>{0, 0, 255})); // outside it

    // --- multi-rect damage: two dirty corners cost two scissors, and the gap
    //     between them keeps the pixels it already had ---
    std::vector<std::uint8_t> green(4);
    green[1] = 255;
    green[3] = 255;
    auto green_tex = renderer->upload_texture(1, 1, green);
    assert(green_tex.has_value());

    // Start from a known full frame: solid blue everywhere.
    status = renderer->render_to(*target, luminaria::Color{0, 0, 1, 1}, {}, {});
    assert(status.has_value());

    // Now paint green over the whole target but only allow two 2x2 corners to
    // be touched. A bounding box would have swallowed the middle as well.
    const luminaria::GpuTextureFill cover{&*green_tex, 0, 0, kW, kH};
    const luminaria::Box dmg[2] = {{0, 0, 2, 2}, {6, 6, 2, 2}};
    status = renderer->render_to(*target, luminaria::Color{0, 0, 0, 1}, {}, {&cover, 1}, dmg);
    assert(status.has_value());
    pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                     p.modifier);
    assert(pixels.has_value());
    assert((at(1, 1) == std::vector<std::uint8_t>{0, 255, 0})); // repainted
    assert((at(7, 7) == std::vector<std::uint8_t>{0, 255, 0})); // repainted
    assert((at(4, 4) == std::vector<std::uint8_t>{0, 0, 255})); // between: untouched blue
    assert((at(1, 7) == std::vector<std::uint8_t>{0, 0, 255})); // in the bbox, not the damage

    // --- occlusion: an opaque surface in front means the one behind is never
    //     sampled, so painting red under an opaque green leaves green ---
    const luminaria::GpuTextureFill behind{&*texture, 0, 0, kW, kH};
    luminaria::GpuTextureFill front{&*green_tex, 0, 0, kW, kH};
    const luminaria::Box all_opaque[1] = {{0, 0, kW, kH}};
    front.opaque = all_opaque;
    const luminaria::GpuTextureFill both[2] = {behind, front};
    status = renderer->render_to(*target, luminaria::Color{1, 0, 1, 1}, {}, both);
    assert(status.has_value());
    pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                     p.modifier);
    assert(pixels.has_value());
    assert((at(4, 4) == std::vector<std::uint8_t>{0, 255, 0}));

    // --- a partial opaque region: what it does NOT claim must keep showing what
    //     is behind it. This is the rounded-corner case — a bounding box would
    //     claim the whole surface and cull the wallpaper under the corners. ---
    // Left pixel opaque green, right pixel fully transparent.
    std::vector<std::uint8_t> half(2 * 1 * 4);
    half[1] = 255; // left: green
    half[3] = 255; // left: alpha 255
    half[7] = 0;   // right: alpha 0
    auto half_tex = renderer->upload_texture(2, 1, half);
    assert(half_tex.has_value());

    luminaria::GpuTextureFill notched{&*half_tex, 0, 0, kW, kH};
    // Only the left half is promised opaque; the right half is see-through.
    const luminaria::Box half_opaque[1] = {{0, 0, kW / 2, kH}};
    notched.opaque = half_opaque;
    const luminaria::GpuTextureFill layered[2] = {behind, notched};
    status = renderer->render_to(*target, luminaria::Color{0, 0, 1, 1}, {}, layered);
    assert(status.has_value());
    pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                     p.modifier);
    assert(pixels.has_value());
    assert((at(1, 4) == std::vector<std::uint8_t>{0, 255, 0})); // claimed: green on top
    // Not claimed: the red surface behind was still drawn and shows through.
    // With a bounding-box opaque this would have been culled to the blue clear.
    assert((at(6, 4) == std::vector<std::uint8_t>{255, 0, 0}));

    // --- a client buffer stored rotated: the same texture, declared as 90
    //     degrees off, must land the other way round ---
    // The source is a 2x1 half-red/half-green strip, so orientation is visible.
    std::vector<std::uint8_t> strip(2 * 1 * 4);
    strip[0] = 255; // left pixel red
    strip[3] = 255;
    strip[5] = 255; // right pixel green
    strip[7] = 255;
    auto strip_tex = renderer->upload_texture(2, 1, strip);
    assert(strip_tex.has_value());
    luminaria::GpuTextureFill rotated{&*strip_tex, 0, 0, kW, kH};
    rotated.transform = luminaria::Transform::rotate_90; // buffer -> surface
    status = renderer->render_to(*target, luminaria::Color{0, 0, 0, 1}, {}, {&rotated, 1});
    assert(status.has_value());
    pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                     p.modifier);
    assert(pixels.has_value());
    // Rotating the strip 90 degrees clockwise turns its left edge into the top
    // edge: red on top, green underneath.
    assert((at(4, 1) == std::vector<std::uint8_t>{255, 0, 0}));
    assert((at(4, 6) == std::vector<std::uint8_t>{0, 255, 0}));

    // --- read_scanout: the cheap read-back path (no re-import, staging buffer
    //     kept and mapped). Must agree with import_dmabuf pixel for pixel. ---
    status = renderer->render_to(*target, luminaria::Color{0, 0, 1, 1}, {}, {&fill, 1});
    assert(status.has_value());
    std::vector<std::uint8_t> direct;
    auto read = renderer->read_scanout(*target, direct);
    assert(read.has_value());
    assert(direct.size() == static_cast<size_t>(kW) * kH * 4);
    pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                     p.modifier);
    assert(pixels.has_value());
    assert(direct == *pixels);
    // Reading twice must give the same thing — the second call reuses the
    // staging buffer, and leaving the image in the wrong layout would show here.
    std::vector<std::uint8_t> again;
    assert(renderer->read_scanout(*target, again).has_value());
    assert(again == direct);
    // ...and rendering still works after a read, i.e. the layout was restored.
    status = renderer->render_to(*target, luminaria::Color{0, 1, 0, 1}, {}, {});
    assert(status.has_value());
    assert(renderer->read_scanout(*target, again).has_value());
    assert((std::vector<std::uint8_t>{again[0], again[1], again[2]} ==
            std::vector<std::uint8_t>{0, 255, 0}));

    // --- explicit sync: ask for an out-fence instead of a CPU stall ---
    int out_fence = -1;
    const luminaria::RenderSync sync{{}, &out_fence};
    status = renderer->render_to(*target, luminaria::Color{0, 0, 1, 1}, {}, {&fill, 1}, {}, {},
                                 sync);
    assert(status.has_value());
    if (out_fence >= 0) {
        // A sync_file that signals when the render lands. Feed it back in as the
        // target's acquire fence: the next render waits on the GPU, not here.
        target->set_acquire_fence(out_fence);
        status = renderer->render_to(*target, luminaria::Color{0, 0, 1, 1}, {}, {&fill, 1});
        assert(status.has_value());
        pixels = renderer->import_dmabuf(p.fd, p.width, p.height, p.format, p.offset, p.stride,
                                         p.modifier);
        assert(pixels.has_value());
        assert((at(1, 1) == std::vector<std::uint8_t>{255, 0, 0}));
    } else {
        std::fprintf(stderr, "note: no VK_KHR_external_semaphore_fd; sync path untested\n");
    }
    return 0;
}
