#version 450
// Water shading: the reflection texture (rendered in the reflection pass this
// frame, mirrored about this exact plane) sampled with a wobble, mixed with the
// pond's own colour by Fresnel, plus the sun's highlight and a foam line where
// the water meets the sand.
//
// The normal comes from screen-space derivatives of the world position rather
// than from a recomputed wave function, so the highlight follows the actual
// displaced surface for three instructions.

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

layout(set = 1, binding = 0) uniform sampler2D u_tex[4];

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main()
{
    const uint b = pc.const_off >> 4;
    const vec4 P = p[b + 0];

    vec3 N = normalize(cross(dFdx(v_world), dFdy(v_world)));
    if (N.y < 0.0) N = -N;

    const vec3 V = normalize(fr.campos.xyz - v_world);

    // ---- reflection: screen-space, wobbled by the wave normal -------------
    const vec2 uv = gl_FragCoord.xy / fr.time.zw + N.xz * 0.05;
    vec3 refl = texture(u_tex[0], clamp(uv, vec2(0.001), vec2(0.999))).rgb;

    // ---- the pond's own colour: deep in the middle, sand through the edge --
    const float shallow = smoothstep(0.35, 1.0, length(v_uv - 0.5) * 2.0);
    const vec3 deep    = vec3(0.05, 0.20, 0.30);
    const vec3 shore   = vec3(0.20, 0.46, 0.44);
    vec3 body = mix(deep, shore, shallow);

    // A stylised Fresnel: the physical 2% at this angle is a black sea, so the
    // base term is lifted until the water always carries some of the sky.
    const float fres = 0.14 + 0.86 * pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0);
    vec3 col = mix(body, refl, clamp(fres, 0.0, 1.0));
    col += body * fr.sky.rgb * 0.35;                 // sky bouncing off the body

    // ---- sun highlight ---------------------------------------------------
    const vec3 H = normalize(fr.sun.xyz + V);
    const float spec = pow(clamp(dot(N, H), 0.0, 1.0), 90.0);
    col += fr.light.rgb * spec * fr.sun.w * 1.6;

    // ---- foam at the shore ----------------------------------------------
    const float foam = smoothstep(0.93, 1.0, length(v_uv - 0.5) * 2.0) *
                       (0.55 + 0.45 * sin(fr.time.x * 2.0 + v_world.x * 6.0));
    col = mix(col, vec3(0.85, 0.90, 0.88), foam * 0.55);

    // ---- fog, matching the terrain --------------------------------------
    const float dist = length(v_world - fr.campos.xyz);
    col = mix(col, fr.horizon.rgb, clamp(1.0 - exp(-dist * fr.sky.a * 0.055), 0.0, 1.0));

    o_color = vec4(col, mix(0.82, 1.0, max(fres, foam)));
}
