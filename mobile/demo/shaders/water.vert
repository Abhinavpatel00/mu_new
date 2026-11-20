#version 450
// The pond: one disc mesh, waves displaced in the vertex shader, and the
// reflection sampled from the texture the reflection pass rendered a moment
// earlier in the same frame. Two passes, one texture, no readback.
//
// Params (16 B): pond_x, pond_z, radius, water level.

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
layout(location = 1) out vec2 v_uv;        // 0..1 across the pond

void main()
{
    const uint b = pc.const_off >> 4;
    const vec4 P = p[b + 0];               // cx, cz, radius, level

    vec3 w = in_pos;
    const float r = length(w.xz - P.xy);
    const float edge = 1.0 - smoothstep(P.z * 0.55, P.z, r);   // flat at the shore

    const float t = fr.time.x;
    const float wave = sin(r * 2.6 - t * 1.9) * 0.5 + sin(w.x * 2.1 + t * 1.2) * 0.3 +
                       sin(w.z * 3.4 - t * 1.5) * 0.2;
    w.y = P.w + wave * 0.045 * edge;

    gl_Position = fr.viewproj * vec4(w, 1.0);
    v_world = w;
    v_uv    = (w.xz - P.xy) / (2.0 * P.z) + 0.5;
}
