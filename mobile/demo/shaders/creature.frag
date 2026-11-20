#version 450
// A creature, drawn as a silhouette: a body ellipse with ears, two eyes and a
// soft contact shadow at the feet. The species changes the proportions and the
// colour; nothing here is a texture and nothing is a mesh.
//
// The shadow is part of the same quad — a dark ellipse at the bottom of the
// billboard — which is why a creature costs one draw instead of two.

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
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_meta;
layout(location = 3) in vec3 v_world;
layout(location = 0) out vec4 o_color;

float sd_ellipse(vec2 p, vec2 r)
{
    return (length(p / r) - 1.0) * min(r.x, r.y);
}

void main()
{
    const vec2 uv = v_uv;
    const float kind = v_meta.x;
    const float facing = v_meta.y;

    // species proportions: tall/short, wide/thin, ears or no ears
    const float wide   = kind < 0.5 ? 1.0 : (kind < 1.5 ? 0.86 : (kind < 2.5 ? 0.78 : 1.15));
    const float tall   = kind < 0.5 ? 1.0 : (kind < 1.5 ? 0.92 : (kind < 2.5 ? 1.22 : 0.82));
    const float ears   = kind < 2.5 ? 1.0 : 0.0;

    // ---- contact shadow, at the feet -------------------------------------
    const vec2 sp = vec2((uv.x - 0.5) * 2.0, (uv.y - 0.06) * 3.0);
    const float sh = 1.0 - smoothstep(0.0, 0.32, length(sp));

    // ---- body ------------------------------------------------------------
    const vec2 bc = vec2((uv.x - 0.5) * 2.0 / wide, (uv.y - 0.42) * 2.0 / tall);
    float body = sd_ellipse(bc, vec2(0.78, 0.62));

    // head: a smaller circle on top, offset toward the facing direction
    const vec2 hc = vec2((uv.x - 0.5 - facing * 0.16) * 2.0 / wide, (uv.y - 0.70) * 2.0 / tall);
    body = min(body, sd_ellipse(hc, vec2(0.46, 0.40)));

    // ears
    if (ears > 0.5) {
        const vec2 e1 = vec2((uv.x - 0.5 - 0.30) * 2.0, (uv.y - 0.88) * 2.0);
        const vec2 e2 = vec2((uv.x - 0.5 + 0.30) * 2.0, (uv.y - 0.88) * 2.0);
        body = min(body, sd_ellipse(e1, vec2(0.16, 0.22)));
        body = min(body, sd_ellipse(e2, vec2(0.16, 0.22)));
    }

    const float cover = 1.0 - smoothstep(-0.02, 0.02, body);

    // ---- eyes ------------------------------------------------------------
    const vec2 ec = vec2((uv.x - 0.5 - facing * 0.26) * 2.0, (uv.y - 0.74) * 2.0);
    const float eye = 1.0 - smoothstep(0.07, 0.10, length(ec));

    // ---- shading: the same sun as everything else ------------------------
    const float wrap = clamp(dot(normalize(vec3(uv.x - 0.5, uv.y - 0.5, 0.8)), fr.sun.xyz) * 0.5 + 0.5,
                             0.0, 1.0);
    vec3 col = v_color.rgb * (0.55 + 0.55 * wrap) *
               (fr.light.rgb * fr.sun.w + fr.light.a + 0.15);

    col = mix(col, vec3(0.06, 0.05, 0.07), eye * cover);
    // rim light on the sun side, so the silhouette reads at any time of day
    col += v_color.rgb * pow(clamp(1.0 - abs(body), 0.0, 1.0), 6.0) * fr.sun.w * 0.5;

    const float dist = length(v_world - fr.campos.xyz);
    col = mix(col, fr.horizon.rgb, clamp(1.0 - exp(-dist * fr.sky.a * 0.05), 0.0, 1.0));

    const vec3 shadow = vec3(0.02, 0.03, 0.04);
    col = mix(col, shadow, sh * 0.45 * (1.0 - cover));

    const float alpha = max(cover, sh * 0.45) * v_color.a;
    if (alpha < 0.02) discard;

    o_color = vec4(1.0, 0.0, 1.0, 1.0);   // DEBUG: solid magenta
}
