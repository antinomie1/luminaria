#version 450
// Dual Kawase, upsampling half. Eight taps in a ring — the diagonals at weight
// 2, the axes at 1, normalised by 12 — which is what makes the result look like
// a Gaussian rather than like a box filter run several times.
//
// The last upsample also does the two things that are cheaper here than in a
// pass of their own, and both are no-ops at their defaults:
//
//   saturation  a blur averages colours towards grey, so the backdrop behind a
//               translucent window comes out duller than the desktop it is part
//               of. Pushing saturation back up is the standard correction.
//   noise       banding. A smooth gradient stretched back to full resolution
//               quantises into visible steps in 8-bit; a per-pixel dither below
//               one LSB is what breaks them up.
//
// Both operate on straight colour, which is safe because a blur source is an
// opaque backdrop — there is nothing to un-premultiply.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv01;
    vec4 uv23;
    float alpha;
    vec4 effect;
    vec4 shape;
    vec4 params; // xy: half-texel offset in UV units, z: noise, w: saturation
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

// Interleaved gradient noise: one dot product and a fract, and it is the
// hash that stays uncorrelated with the pixel grid at any resolution.
float dither(vec2 position) {
    return fract(52.9829189 * fract(dot(position, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec2 o = pc.params.xy;
    vec4 sum = texture(tex, v_uv + vec2(-o.x * 2.0, 0.0));
    sum += texture(tex, v_uv + vec2(-o.x, o.y)) * 2.0;
    sum += texture(tex, v_uv + vec2(0.0, o.y * 2.0));
    sum += texture(tex, v_uv + vec2(o.x, o.y)) * 2.0;
    sum += texture(tex, v_uv + vec2(o.x * 2.0, 0.0));
    sum += texture(tex, v_uv + vec2(o.x, -o.y)) * 2.0;
    sum += texture(tex, v_uv + vec2(0.0, -o.y * 2.0));
    sum += texture(tex, v_uv + vec2(-o.x, -o.y)) * 2.0;
    vec4 blurred = sum / 12.0;

    // Rec. 709 luma, the same weights the rest of the desktop's colour maths
    // uses; mixing away from it is a saturation, mixing past 1 boosts it.
    vec3 rgb = blurred.rgb;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luma), rgb, pc.params.w);
    rgb += (dither(gl_FragCoord.xy) - 0.5) * pc.params.z;

    color = vec4(clamp(rgb, 0.0, 1.0), blurred.a);
}
