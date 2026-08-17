#version 450
// Dual Kawase, downsampling half. Five taps into a texture twice this one's
// size: the centre at weight 4 and the four diagonals at 1, normalised by 8.
//
// The trick the whole algorithm rests on is that the samples sit *between*
// texels, so the bilinear filter the sampler already runs turns each tap into
// an average of four. Five fetches therefore carry the information of twenty,
// which is why a Kawase blur of a given radius costs a fraction of a Gaussian
// of the same radius and is what every compositor doing live blur uses.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv01;
    vec4 uv23;
    float alpha;
    vec4 effect;
    vec4 shape;
    vec4 params; // xy: half-texel offset in UV units, zw: unused here
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    vec2 o = pc.params.xy;
    vec4 sum = texture(tex, v_uv) * 4.0;
    sum += texture(tex, v_uv - o);
    sum += texture(tex, v_uv + o);
    sum += texture(tex, v_uv + vec2(o.x, -o.y));
    sum += texture(tex, v_uv - vec2(o.x, -o.y));
    color = sum / 8.0;
}
