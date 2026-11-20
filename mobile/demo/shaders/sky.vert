#version 450
// Sky: a fullscreen triangle, generated from gl_VertexIndex, shaded from the
// frame constants alone. Gradient by screen height, a sun disc and its glow at
// the projected sun position, and stars that only appear when the sun is down.
// No geometry, no texture, one draw.

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

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    // A hair inside the far plane, not on it: the depth buffer is cleared to
    // 1.0 and the test is LESS, so z = 1.0 would fail against the clear and the
    // sky would never draw a pixel (bug found by screenshot: the whole frame
    // was the pass clear colour). At 0.9999 the sky still beats the clear and
    // still loses to anything real, so it needs no ordering at all -- it only
    // fills the pixels nothing else claimed, first or last.
    gl_Position = vec4(p * 2.0 - 1.0, 0.9999, 1.0);
    v_uv = p;
}
