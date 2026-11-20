#version 450
// Rounded box in screen space, anti-aliased from the signed distance.
// No texture fetch, no sampler: a game layer of solid shapes is the cheapest
// fragment work there is, and the SDF gives the rounded corners and the inner
// rim for free instead of shipping a nine-patch.

layout(push_constant) uniform Push { uint const_off; uint flags; } pc;

layout(std430, set = 0, binding = 1) readonly buffer Params { vec4 p[]; };

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_shape;   // radius, border, glow, -
layout(location = 3) in vec2 v_half;

layout(location = 0) out vec4 o_color;

void main()
{
    const float radius = min(v_shape.x, min(v_half.x, v_half.y));
    const vec2  d      = (v_uv * 2.0 - 1.0) * v_half;         // px from centre
    const vec2  q      = abs(d) - (v_half - radius);
    const float sd     = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;

    // 1.2 px of coverage: the minimum that is smooth at 1x and free at 4x
    float alpha = 1.0 - smoothstep(-1.2, 0.8, sd);
    if (alpha <= 0.0) discard;

    // inner rim: brighter within `border` px of the edge, for depth
    const float inside = -sd;
    const float rim    = v_shape.y > 0.0 ? 1.0 - smoothstep(0.0, v_shape.y, inside) : 0.0;

    vec3 c = v_color.rgb + rim * v_shape.z * (0.55 * v_color.rgb + 0.25);
    o_color = vec4(c, v_color.a * alpha);
}
