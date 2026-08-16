#version 450
// Solid-colour quad: the vertex stage positions it, this stage paints a
// constant pre-multiplied colour carried in the push constants — no texture
// binding at all. Compositor-owned rectangles (borders, masks, cursor
// backdrops) travel the same ordered-fill path as surfaces, so they cannot
// disagree with the z-order or the damage.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv01;   // solid colour, pre-multiplied by its alpha
    vec4 uv23;
    float alpha; // solid alpha
} pc;

layout(location = 0) out vec4 color;

void main() {
    color = vec4(pc.uv01.rgb, pc.alpha);
}
