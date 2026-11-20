#version 450
// Island shading: sand where it is low and flat, grass above it, rock on the
// steep faces, all lit by one sun and fogged into the horizon colour. Nothing
// here samples a texture — an island is a heightfield, and a heightfield can
// carry its own colour.
//
// Waterline foam is a band around fr.horizon.a, which is also the plane the
// reflection pass mirrors about: one number defines the pond.

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
layout(location = 0) out vec4 o_color;

void main()
{
    const vec3 N = normalize(v_normal);
    const float level = fr.horizon.a;
    const float h = v_world.y - level;               // height above the water

    // ---- albedo by height and slope --------------------------------------
    const vec3 sand  = vec3(0.78, 0.70, 0.50);
    const vec3 grass = vec3(0.30, 0.52, 0.26);
    const vec3 moss  = vec3(0.22, 0.40, 0.22);
    const vec3 rock  = vec3(0.42, 0.40, 0.38);

    const float flatness = clamp(N.y, 0.0, 1.0);   // 'flat' is a GLSL keyword
    vec3 albedo = mix(sand, grass, smoothstep(0.10, 0.55, h));
    albedo = mix(albedo, moss, smoothstep(1.3, 2.4, h) * 0.75);
    albedo = mix(albedo, rock, smoothstep(0.92, 0.62, flatness));

    // wet sand right at the waterline
    albedo *= mix(0.72, 1.0, smoothstep(-0.15, 0.25, h));

    // far side of the island: slightly bluer, it is in the haze
    // ---- lighting: one sun with a soft terminator, plus ambient ----------
    const float ndl  = dot(N, fr.sun.xyz);
    const float wrap = clamp(ndl * 0.5 + 0.5, 0.0, 1.0);
    vec3 col = albedo * (fr.light.rgb * wrap * fr.sun.w + fr.light.a);

    // a little bounce from the sky, so shadowed sand is not black
    col += albedo * fr.sky.rgb * 0.10;

    // ---- fog by distance -------------------------------------------------
    const float dist = length(v_world - fr.campos.xyz);
    const float fog  = 1.0 - exp(-dist * fr.sky.a * 0.055);
    col = mix(col, fr.horizon.rgb, clamp(fog, 0.0, 1.0));

    o_color = vec4(col, 1.0);
}
