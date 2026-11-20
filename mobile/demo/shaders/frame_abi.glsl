#version 450
// ============================================================
// THE FRAME ABI — every 3D shader in the demo declares this prefix
// ============================================================
// One uniform buffer for the whole frame (set 0, binding 0), 208 of the 256
// bytes mg reserves for it. There is no per-draw camera, no per-object matrix:
// a frame has one sun, one camera and one sky, so they live here once.
//
//   0..15  viewproj
//  16..19  time      x seconds, y aspect, z width px, w height px
//  20..23  sun       xyz unit vector toward the sun, w intensity
//  24..27  light     rgb sun colour, a ambient
//  28..31  sky       rgb zenith colour, a fog density
//  32..35  horizon   rgb horizon colour, a water level (world y)
//  36..39  campos    camera position
//  40..43  camright  camera right (billboards)
//  44..47  camup     camera up (billboards)
//  48..51  sunndc    xy sun on screen, z sun radius, w night factor
//
// (This block is pasted into each shader because glslc has no include path
// here; it is the same twelve lines everywhere and must stay byte-identical.)

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
