#version 450
// Frame-time HUD, vertex stage. One quad, four vertices, 6 indices: the panel
// is placed in pixels from its parameter slot, so the overlay costs one draw
// call and 512 bytes of CPU writes per frame, and no geometry or texture.

layout(push_constant) uniform Push { uint const_off; uint flags; } pc;

layout(set = 0, binding = 0) uniform Frame { mat4 viewproj; vec4 time; } fr;
layout(std430, set = 0, binding = 1) readonly buffer Params { vec4 p[]; };

layout(location = 0) in vec3 in_pos;      // unit quad, 0..1

layout(location = 0) out vec2 v_uv;

void main()
{
    const uint b = pc.const_off >> 4;
    const vec4 r = p[b + 0];                       // px, py, pw, ph (pixels)
    const vec2 px = r.xy + in_pos.xy * r.zw;
    const vec2 ndc = px / fr.time.zw * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);      // Vulkan clip space is y down
    v_uv = in_pos.xy;
}
