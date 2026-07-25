#version 450
// Straight texture fetch. Blending is fixed-function: Wayland buffers carry
// pre-multiplied alpha, so the pipeline uses ONE / ONE_MINUS_SRC_ALPHA and an
// opaque surface is handled by swizzling its view's alpha to 1.

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    color = texture(tex, v_uv);
}
