#version 450
// A drop shadow: one solid quad whose alpha falls off with the distance to a
// rounded box. No texture, no blur pass, no extra render target — the shape is
// analytic, so a shadow costs the same as the rectangle it replaces.
//
// That is why shadows do not go through the blur chain even though a shadow is
// conceptually a blurred silhouette: the silhouette is known exactly here, and
// blurring a known shape is paying for an approximation of something already in
// closed form.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv01;   // rgb: shadow colour, premultiplied
    vec4 uv23;
    float alpha; // shadow colour's alpha
    vec4 effect; // x: corner radius in device px, y: falloff distance in device px
    vec4 shape;  // xy: casting box centre in device px, zw: its half extent
} pc;

layout(location = 0) out vec4 color;

float rounded_box_sdf(vec2 point, vec2 half_extent, float radius) {
    float r = min(radius, min(half_extent.x, half_extent.y));
    vec2 q = abs(point) - half_extent + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

void main() {
    float d = rounded_box_sdf(gl_FragCoord.xy - pc.shape.xy, pc.shape.zw, pc.effect.x);
    // smoothstep rather than a linear ramp: a linear falloff has a visible
    // crease where it meets the window edge, which reads as a drawn outline.
    float falloff = 1.0 - smoothstep(0.0, max(pc.effect.y, 1.0), d);
    // Nothing inside the casting box. A window is drawn over its own shadow, so
    // the fill would be invisible if it were opaque — and visibly wrong if it
    // is not, since a window must not be darkened by the shadow it casts.
    float outside = smoothstep(-0.5, 0.5, d);
    color = vec4(pc.uv01.rgb, pc.alpha) * (falloff * outside);
}
