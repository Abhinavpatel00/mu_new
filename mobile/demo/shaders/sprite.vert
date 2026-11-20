#version 450
// 2D sprite: a unit quad (four vertices, one stream, 12 B each) placed by
// parameters. Nothing is fetched per sprite except those four positions, and
// the whole quad is 6 indices, so a paddle, a ball or a brick costs one draw
// record, one 8-byte push and one 48-byte param slot.
//
// Params for a sprite (48 B, one cache line):
//   p[0] = cx, cy, half_w, half_h      pixels, origin top-left, y down
//   p[1] = r, g, b, a
//   p[2] = corner_radius_px, border_px, angle_rad, glow

layout(push_constant) uniform Push { uint const_off; uint flags; } pc;

layout(set = 0, binding = 0) uniform Frame { mat4 viewproj; vec4 time; } fr;
layout(std430, set = 0, binding = 1) readonly buffer Params { vec4 p[]; };

layout(location = 0) in vec3 in_pos;      // unit quad, 0..1

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_shape;    // radius, border, glow, -
layout(location = 3) out vec2 v_half;

void main()
{
    const uint b = pc.const_off >> 4;
    const vec4 r = p[b + 0];
    const vec4 s = p[b + 2];

    vec2 q = (in_pos.xy * 2.0 - 1.0) * r.zw;          // px from the centre
    const float ca = cos(s.z), sa = sin(s.z);
    q = vec2(q.x * ca - q.y * sa, q.x * sa + q.y * ca);

    const vec2 px = r.xy + q;
    // fr.time.zw is the target size in pixels: one multiply and one add per
    // vertex, and no matrix, because a 2D layer has no camera.
    const vec2 ndc = px / fr.time.zw * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);       // Vulkan clip space is y down

    v_uv    = in_pos.xy;
    v_color = p[b + 1];
    v_shape = vec4(s.x, s.y, s.w, 0.0);
    v_half  = r.zw;
}
