#version 450
// Scene pass, fragment stage. A material is set 1: four statically bound
// combined image samplers. No bindless, no descriptor indexing.

layout(set = 1, binding = 0) uniform sampler2D u_tex[4];

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_local;

layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(u_tex[0], v_uv);
    float rim = smoothstep(0.0, 0.9, length(v_local));
    o_color = vec4(t.rgb * v_color.rgb * (0.55 + 0.45 * rim), v_color.a * t.a);
}
