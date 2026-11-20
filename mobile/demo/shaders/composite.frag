#version 450
// Composite: the resolved MSAA scene (material slot 0), vignette + gamma.
// It samples an attachment written by the previous pass, which only works
// because that pass declares SHADER_READ_ONLY as the resolve's final layout:
// the pass carries the synchronisation, the application never does.

layout(set = 1, binding = 0) uniform sampler2D u_tex[4];

layout(location = 0) out vec4 o_color;

void main()
{
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(u_tex[0], 0));
    vec3 c = texture(u_tex[0], uv).rgb;
    vec2 d = uv * 2.0 - 1.0;
    c *= 1.0 - 0.35 * dot(d, d);
    o_color = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
