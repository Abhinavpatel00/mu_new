#version 450
// Grass: ~900 blades in ONE mesh and ONE draw. The blade's own attribute stream
// carries (phase, height fraction) instead of UVs, so the wind is a function of
// the vertex: the tip moves, the root does not, and neighbouring blades differ
// because their phases differ. A second, slower wave moves through the field so
// the gusts do not look like a single sine.

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
layout(location = 1) in vec2 in_attr;      // x phase (0..1), y height fraction (0..1)

layout(location = 0) out vec3 v_world;
layout(location = 1) out float v_h;        // height fraction: the colour ramp
layout(location = 2) out float v_tip;      // 0 flat, 1 bent

void main()
{
    vec3 p = in_pos;
    const float hf = in_attr.y;
    const float bend = hf * hf;

    const float gust  = sin(fr.time.x * 1.1 + p.x * 0.35 + p.z * 0.22);
    const float wave  = sin(fr.time.x * 2.7 + in_attr.x * 6.2831 + p.x * 1.3 + p.z * 0.9);
    const float sway  = (0.55 + 0.45 * gust) * wave;

    p.x += sway * 0.10 * bend;
    p.z += cos(fr.time.x * 1.9 + in_attr.x * 6.2831 + p.z * 1.1) * 0.05 * bend;
    p.y -= abs(sway) * 0.03 * bend;

#if defined(MIRROR)
    p.y = 2.0 * fr.horizon.a - p.y;
#endif

    gl_Position = fr.viewproj * vec4(p, 1.0);
    v_world = p;
    v_h     = hf;
    v_tip   = bend;
}
