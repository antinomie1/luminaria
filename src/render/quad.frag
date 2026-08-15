#version 450
// Straight texture fetch. Blending is fixed-function: Wayland buffers carry
// pre-multiplied alpha, so the pipeline uses ONE / ONE_MINUS_SRC_ALPHA and an
// opaque surface is handled by swizzling its view's alpha to 1.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv01;
    vec4 uv23;
    float alpha; // whole-quad opacity; 1.0 for an ordinary surface
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    // Scaling colour and alpha by the same factor keeps the sample
    // premultiplied, which is what the fixed-function blend below expects.
    color = texture(tex, v_uv) * pc.alpha;
}
