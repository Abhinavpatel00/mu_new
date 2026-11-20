#version 450
// Prop shading: bark below the crown line, leaf colour above it, stone for the
// rocks, all with the frame's sun and fog. The grain is a cheap hash on the
// local position — a texture would cost a sampler and an upload for something
// that only has to break up a flat colour.

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
layout(location = 1) in vec3 v_normal;
layout(location = 2) in float v_local_y;
layout(location = 3) in vec4 v_color;
layout(location = 4) in float v_kind;
layout(location = 0) out vec4 o_color;

void main()
{
    const vec3 N = normalize(v_normal);
    const float kind = v_kind;

    vec3 albedo;
    if (kind > 1.5) {
        albedo = v_color.rgb * (0.85 + 0.15 * sin(v_local_y * 9.0));
    } else if (kind > 0.5) {
        albedo = v_color.rgb * (0.88 + 0.12 * sin(v_world.y * 18.0));
    } else {
        const float crown = smoothstep(0.62, 0.95, v_local_y);      // trunk -> leaves
        const vec3 bark = vec3(0.28, 0.20, 0.14) * (0.9 + 0.1 * sin(v_local_y * 26.0));
        vec3 leaf = v_color.rgb * (0.82 + 0.18 * sin(v_world.x * 3.1 + v_world.z * 2.7));
        albedo = mix(bark, leaf, crown) * mix(0.55, 1.0, v_color.a);  // growth
    }

    const float wrap = clamp(dot(N, fr.sun.xyz) * 0.5 + 0.5, 0.0, 1.0);
    vec3 col = albedo * (fr.light.rgb * wrap * fr.sun.w + fr.light.a);
    col += albedo * fr.sky.rgb * 0.12;

    const float dist = length(v_world - fr.campos.xyz);
    col = mix(col, fr.horizon.rgb, clamp(1.0 - exp(-dist * fr.sky.a * 0.055), 0.0, 1.0));

    o_color = vec4(col, 1.0);
}
