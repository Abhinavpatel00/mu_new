#version 450
// Island terrain: one mesh in world space, positions 12 B and ONE 8-byte
// attribute — the normal in octahedral form (mg.h's vertex-fetch rule: two
// streams, 12 B + 8 B, and never a third).
//
// Compiled twice: plain, and with -DMIRROR=1 for the reflection pass, where the
// island is flipped about the water plane and drawn with the SAME camera.

layout(push_constant) uniform Push { uint const_off; uint flags; } pc;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewproj;
    vec4 time;
    vec4 sun;
    vec4 light;
    vec4 sky;
    vec4 horizon;
    vec4 campos;
    vec4 camright;
    vec4 camup;
    vec4 sunndc;
} fr;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_nrm;      // octahedral normal

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;

vec3 oct_decode(vec2 e)
{
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}

void main()
{
    vec3 p = in_pos;
    vec3 n = oct_decode(in_nrm);
#ifdef MIRROR
    p.y = 2.0 * fr.horizon.a - p.y;       // reflect about the water plane
    n.y = -n.y;
#endif
    gl_Position = fr.viewproj * vec4(p, 1.0);
    v_world  = p;
    v_normal = n;
}
