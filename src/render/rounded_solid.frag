#version 450
// A solid quad cut to a rounded box, filled or as a ring. The same signed distance
// as rounded.frag and shadow.frag, so a rounded window, its border and its shadow
// agree about where the corner is. A ring exists because a window's border must be
// hollow: anything solid behind a translucent window shows through it, and ends up
// in its blur.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv01;   // rgb: colour, premultiplied
    vec4 uv23;
    float alpha; // colour's alpha
    vec4 effect; // x: corner radius in device px, z: ring thickness, 0 for filled
    vec4 shape;  // xy: quad centre in device px, zw: half extent in device px
} pc;

layout(location = 0) out vec4 color;

float rounded_box_sdf(vec2 point, vec2 half_extent, float radius) {
    float r = min(radius, min(half_extent.x, half_extent.y));
    vec2 q = abs(point) - half_extent + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

void main() {
    float d = rounded_box_sdf(gl_FragCoord.xy - pc.shape.xy, pc.shape.zw, pc.effect.x);
    float coverage = 1.0 - smoothstep(-0.5, 0.5, d);
    if (pc.effect.z > 0.0) {
        coverage *= smoothstep(-0.5, 0.5, d + pc.effect.z);
    }
    color = vec4(pc.uv01.rgb, pc.alpha) * coverage;
}
