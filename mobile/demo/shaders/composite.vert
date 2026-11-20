#version 450
// Composite pass: a fullscreen triangle generated from gl_VertexIndex.
// Nothing fetched, nothing pulled -- the cheapest vertex work there is, and
// the one place "generate from the index" genuinely wins.

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
