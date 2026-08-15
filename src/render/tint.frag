#version 450
// Test-only custom fragment stage. It uses exactly the ABI custom compositor
// shaders must keep: the quad vertex stage supplies v_uv; descriptor set 0 is
// the source texture; and Push stays byte-for-byte compatible with quad.vert.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv01;
    vec4 uv23;
    float alpha;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    vec4 source = texture(tex, v_uv) * pc.alpha;
    color = vec4(source.g, source.r, source.b, source.a);
}
