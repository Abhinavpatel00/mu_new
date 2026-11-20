#version 450
// Sky gradient + sun + stars, from the frame constants only.
//
// The sun is not a mesh: the CPU projects its direction to normalized screen
// coordinates once per frame and passes that in fr.sunndc, so the disc, the
// glow and the horizon tint follow it for free. Stars are a hash on the pixel
// grid, faded in by the night factor.

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

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main()
{
    const vec2 uv = gl_FragCoord.xy / fr.time.zw;    // 0 top, 1 bottom

    // ---- gradient: zenith at the top, horizon at the bottom ---------------
    const float t = pow(clamp(1.0 - uv.y, 0.0, 1.0), 0.75);
    vec3 col = mix(fr.horizon.rgb, fr.sky.rgb, t);

    // ---- stars, only when it is dark -------------------------------------
    const float night = fr.sunndc.w;
    if (night > 0.02 && uv.y < 0.62) {
        const vec2 g = floor(gl_FragCoord.xy / 3.0);
        float s = hash(g);
        if (s > 0.9965) {
            const float tw = 0.6 + 0.4 * sin(fr.time.x * 2.0 + s * 90.0);
            col += vec3(0.85, 0.9, 1.0) * night * tw * (1.0 - uv.y / 0.62);
        }
    }

    // ---- the sun (or moon) disc and its glow ------------------------------
    const vec2 d = (uv - fr.sunndc.xy) * vec2(fr.time.y, 1.0);
    const float r = length(d);
    col += fr.light.rgb * exp(-r * 6.0) * 0.35 * max(fr.sun.w, night * 0.5);
    col += mix(fr.light.rgb, vec3(0.85, 0.9, 1.0), night) *
           smoothstep(fr.sunndc.z, fr.sunndc.z * 0.75, r) * 1.4;

    o_color = vec4(col, 1.0);
}
