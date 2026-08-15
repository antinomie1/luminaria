#version 450
// One textured quad, positioned entirely by push constants: no vertex buffer,
// no index buffer, four vertices in a triangle strip. The UVs are handed in per
// corner, which is how output rotation and reflection are applied — the CPU
// decides which source corner each destination corner reads.

layout(push_constant) uniform Push {
    vec4 rect;   // clip space: x0, y0, x1, y1
    vec4 uv01;   // uv of corner 0 (xy) and corner 1 (zw)
    vec4 uv23;   // uv of corner 2 (xy) and corner 3 (zw)
    float alpha; // read by the fragment stage; declared here so the block matches
} pc;

layout(location = 0) out vec2 v_uv;

void main() {
    // Strip order: top-left, top-right, bottom-left, bottom-right.
    vec2 pos[4] = vec2[4](vec2(pc.rect.x, pc.rect.y), vec2(pc.rect.z, pc.rect.y),
                          vec2(pc.rect.x, pc.rect.w), vec2(pc.rect.z, pc.rect.w));
    vec2 uv[4] = vec2[4](pc.uv01.xy, pc.uv01.zw, pc.uv23.xy, pc.uv23.zw);
    v_uv = uv[gl_VertexIndex];
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
