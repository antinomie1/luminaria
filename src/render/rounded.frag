#version 450
// A textured quad with rounded corners. Identical to quad.frag except that the
// sample is multiplied by a rounded-box coverage mask, so the corners of a
// window are cut out of the window itself rather than painted over by the
// compositor — painting over would need to know what is behind them.
//
// The mask is computed in framebuffer pixels from gl_FragCoord, not from v_uv:
// v_uv is the source crop and says nothing about the destination's shape.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv01;
    vec4 uv23;
    float alpha;
    vec4 effect; // x: corner radius in device px
    vec4 shape;  // xy: quad centre in device px, zw: half extent in device px
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

float rounded_box_sdf(vec2 point, vec2 half_extent, float radius) {
    float r = min(radius, min(half_extent.x, half_extent.y));
    vec2 q = abs(point) - half_extent + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

void main() {
    float d = rounded_box_sdf(gl_FragCoord.xy - pc.shape.xy, pc.shape.zw, pc.effect.x);
    // One pixel of coverage across the edge. Scaling the whole premultiplied
    // sample keeps it premultiplied, which is what the fixed-function blend
    // expects — masking only the alpha would leave a bright fringe.
    float coverage = 1.0 - smoothstep(-0.5, 0.5, d);
    color = texture(tex, v_uv) * pc.alpha * coverage;
}
