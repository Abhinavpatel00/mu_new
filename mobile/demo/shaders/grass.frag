#version 450
// Grass shading: a green-to-yellow ramp up the blade, lit by the sun with the
// same soft terminator as the terrain, fogged into the horizon. Double-sided:
// a blade has no back face worth culling, and culling it would make half the
// field disappear depending on the camera yaw.

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

layout(location = 0) in vec3 v_world;
layout(location = 1) in float v_h;
layout(location = 2) in float v_tip;
layout(location = 0) out vec4 o_color;

void main()
{
    const vec3 root = vec3(0.16, 0.30, 0.14);
    const vec3 tip  = vec3(0.55, 0.72, 0.30);
    vec3 albedo = mix(root, tip, v_h) * (1.0 - 0.25 * v_tip);

    // blades face the sun by brute force: no normal, so key on how upright it is
    const float lit = 0.55 + 0.45 * v_h;
    vec3 col = albedo * (fr.light.rgb * lit * fr.sun.w + fr.light.a);
    col += albedo * fr.sky.rgb * 0.18;

    const float dist = length(v_world - fr.campos.xyz);
    col = mix(col, fr.horizon.rgb, clamp(1.0 - exp(-dist * fr.sky.a * 0.06), 0.0, 1.0));

    o_color = vec4(col, 1.0);
}
