#version 450
// Trees, bushes and rocks. One mesh each, one draw each, and the animation is
// entirely in this shader: the wind is a function of time and the vertex's own
// height above the pivot, weighted by the square of that height so the trunk
// stays planted and the crown moves. Nothing is skinned, nothing is rebuilt on
// the CPU, and the same 21-vertex tree is drawn 40 times with different params.
//
// Params (48 B):
//   p[0] = x, y, z, scale
//   p[1] = yaw, sway amplitude, sway phase, kind (0 tree, 1 bush, 2 rock)
//   p[2] = r, g, b, growth                  (rock: the tint of the stone)

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

layout(std430, set = 0, binding = 1) readonly buffer Params { vec4 p[]; };

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_nrm;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out float v_local_y;
layout(location = 3) out vec4 v_color;
layout(location = 4) out float v_kind;

vec3 oct_decode(vec2 e)
{
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}

void main()
{
    const uint b = pc.const_off >> 4;
    const vec4 P = p[b + 0];
    const vec4 Q = p[b + 1];
    const vec4 C = p[b + 2];

    vec3 pos = in_pos * P.w;
    vec3 nrm = oct_decode(in_nrm);

    // sway: more at the top, none at the pivot, and each plant has its phase
    const float weight = clamp(in_pos.y * 1.4, 0.0, 1.0);
    const float w = weight * weight;
    const float t = fr.time.x * 1.6 + Q.z;
    pos.x += (sin(t) * 0.045 + sin(t * 2.3 + 1.7) * 0.012) * Q.y * w * P.w;
    pos.z += (cos(t * 0.8 + 0.4) * 0.035) * Q.y * w * P.w;
    pos.y -= (0.5 - 0.5 * cos(t)) * 0.01 * Q.y * w * P.w;   // nothing gained for free

    // yaw, then place
    const float s = sin(Q.x), c = cos(Q.x);
    pos = vec3(pos.x * c - pos.z * s, pos.y, pos.x * s + pos.z * c);
    nrm = vec3(nrm.x * c - nrm.z * s, nrm.y, nrm.x * s + nrm.z * c);

    vec3 world = pos + P.xyz;
#ifdef MIRROR
    world.y = 2.0 * fr.horizon.a - world.y;
    nrm.y = -nrm.y;
#endif

    gl_Position = fr.viewproj * vec4(world, 1.0);
    v_world   = world;
    v_normal  = nrm;
    v_local_y = in_pos.y;
    v_color   = C;
    v_kind    = Q.w;
}
