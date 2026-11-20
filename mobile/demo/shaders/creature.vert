#version 450
// Creatures are billboards: one quad, one draw each, facing the camera from the
// frame's own right/up vectors. A creature costs 48 bytes of params — position,
// scale, colour, phase, species — and no geometry at all, because all of them
// share the same four corners.
//
// Params (48 B):
//   p[0] = x, y, z, size
//   p[1] = r, g, b, alpha
//   p[2] = phase, kind, bob, facing (+1 / -1)

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

layout(location = 0) in vec3 in_pos;       // unit quad, 0..1, pivot at (0,0)

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_meta;      // kind, facing
layout(location = 3) out vec3 v_world;

void main()
{
    const uint b = pc.const_off >> 4;
    const vec4 P = p[b + 0];
    const vec4 Q = p[b + 2];

    const float bob = sin(fr.time.x * 5.0 + Q.x) * 0.035 * Q.z;
    vec3 center = P.xyz + vec3(0.0, bob, 0.0);

    // billboard: the quad's x along the camera's right, its y along world up
    const vec2 q = vec2(in_pos.x - 0.5, in_pos.y) * P.w * 2.0;
    vec3 world = center + fr.camright.xyz * q.x + vec3(0.0, 1.0, 0.0) * q.y;

#ifdef MIRROR
    world.y = 2.0 * fr.horizon.a - world.y;
#endif

    gl_Position = fr.viewproj * vec4(world, 1.0);
#ifdef MIRROR
    v_uv = vec2(in_pos.x, 1.0 - in_pos.y);    // a reflection is upside down
#else
    v_uv = in_pos.xy;
#endif
    v_color = p[b + 1];
    v_meta  = vec2(Q.y, Q.w);
    v_world = world;
}
