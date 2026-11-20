#version 450
// Scene pass, vertex stage.
// The 8-byte push carries an offset into the param ring and a flag word.
// Geometry arrives through fixed-function vertex fetch, two streams, never by
// indexing a storage buffer -- see MOBILE_API_DESIGN.md 5.5.

layout(push_constant) uniform Push { uint const_off; uint flags; } pc;

layout(set = 0, binding = 0) uniform Frame { mat4 viewproj; vec4 time; } fr;
layout(std430, set = 0, binding = 1) readonly buffer Params { vec4 p[]; };

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_local;

void main()
{
    uint b = pc.const_off >> 4;
    vec4 x = p[b + 0];                 // xy = offset, z = scale, w = spin
    vec4 c = p[b + 1];                 // rgba

    float s = sin(x.w), co = cos(x.w);
    vec2 r = vec2(co * in_pos.x - s * in_pos.y,
                  s * in_pos.x + co * in_pos.y) * x.z + x.xy;

    gl_Position = fr.viewproj * vec4(r, in_pos.z, 1.0);
    v_uv      = in_uv;
    v_color   = c;
    v_local   = in_pos.xy;
}
