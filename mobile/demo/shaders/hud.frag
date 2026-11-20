#version 450
// ============================================================
// THE FRAME-TIME HUD — real text, one draw, no texture
// ============================================================
//
// The font is MicroProfile's 5x8 bitmap font (glyphs courtesy of
// github.com/zeux/microprofile), embedded as bits: a 1024x9 pixel sheet packed
// into 288 uints, one glyph per 5 columns of u, the ninth row being the drop
// shadow. The lookup below is that project's compute shader, moved into a
// fragment shader: same bitfieldExtract of the same tables, same shadow row.
//
// So the overlay costs no texture, no atlas, no upload and no copy — about 1 KB
// of constants and 608 bytes of param slot per frame, and one draw call.
//
//   p[0]     px, py, pw, ph            panel rect in pixels
//   p[1]     scale1, scale2, ms, fps
//   p[2..7]  line 1: 24 characters (ASCII, 0 = end)
//   p[8..13] line 2: 24 characters
//   p[14..37] the graph: 96 frame intervals, 4 per vec4, 0..1
//
// Everything is laid out in units of ph/4, so the panel scales with the display.

layout(push_constant) uniform Push { uint const_off; uint flags; } pc;

layout(std430, set = 0, binding = 1) readonly buffer Params { vec4 p[]; };

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

#define NSAMP     96.0
#define G_LO      14u

// ---- MicroProfile font: glyph offsets for ASCII 0..255 ------------------
const uint g_font_desc[128] = {
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce0201,0x2090211,0x2190221,0x2290231,0x2390241,0x2490251,0x2590261,0x2690271,
    0x1b101b9,0x1c101c9,0x1d101d9,0x1e101e9,0x1f101f9,0x2790281,0x2890291,0x29902a1,
    0x2a90001,0x0090011,0x0190021,0x0290031,0x0390041,0x0490051,0x0590061,0x0690071,
    0x0790081,0x0890091,0x09900a1,0x0a900b1,0x0b900c1,0x0c902b1,0x2b902c1,0x2c902d1,
    0x0ce00d9,0x0e100e9,0x0f100f9,0x1010109,0x1110119,0x1210129,0x1310139,0x1410149,
    0x1510159,0x1610169,0x1710179,0x1810189,0x1910199,0x1a102d9,0x2e102e9,0x2f100ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
    0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,0x0ce00ce,
};

// ---- MicroProfile font: 1024x9 pixels, one bit each ---------------------
const uint g_font_bits[288] = {
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x10783878,0x7c7c3c44,0x38044440,0x44443878,0x3878387c,0x44444444,0x447c0000,0x40000400,
    0x18004010,0x08403000,0x00000000,0x00001000,0x00000000,0x00003810,0x387c087c,0x1c7c3838,
    0x10282810,0x00201008,0x10100000,0x00000000,0x00040020,0x38387000,0x1c10001c,0x10703000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x28444444,0x40404044,0x10044840,0x6c444444,0x44444410,0x44444444,0x44040000,0x40000400,
    0x24004000,0x00401000,0x00000000,0x00001000,0x00000000,0x00004430,0x44041840,0x20044444,
    0x1028283c,0x44501010,0x08541000,0x00000400,0x00080010,0x44444040,0x04280030,0x10185800,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x44444044,0x40404044,0x10045040,0x54644444,0x44444010,0x44444428,0x28080038,0x783c3c38,
    0x20387830,0x1844106c,0x7838783c,0x5c3c3c44,0x44444444,0x7c004c10,0x04082878,0x40084444,
    0x10007c50,0x08500020,0x04381000,0x00000810,0x10107c08,0x08544020,0x04440030,0x10180000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x44784044,0x7878407c,0x10046040,0x54544478,0x44783810,0x44445410,0x10100004,0x44404444,
    0x78444410,0x08481054,0x44444444,0x60401044,0x44442844,0x08005410,0x18184804,0x7810383c,
    0x10002838,0x10200020,0x04107c00,0x7c001000,0x00200004,0x105c4010,0x04000060,0x100c0000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x7c444044,0x40404c44,0x10045040,0x444c4440,0x54500410,0x44445428,0x1020003c,0x4440447c,
    0x20444410,0x08701054,0x44444444,0x40381044,0x44541044,0x10006410,0x20047c04,0x44204404,
    0x10007c14,0x20540020,0x04381010,0x00002010,0x10107c08,0x10584008,0x04000030,0x10180000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x44444444,0x40404444,0x10444840,0x44444440,0x48484410,0x44286c44,0x10400044,0x44404440,
    0x203c4410,0x08481054,0x44444444,0x4004124c,0x2854283c,0x20004410,0x40440844,0x44204408,
    0x00002878,0x44480010,0x08541010,0x00004000,0x10080010,0x00404004,0x04000030,0x10180000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x44783878,0x7c403c44,0x3838447c,0x44443840,0x34443810,0x38104444,0x107c003c,0x783c3c3c,
    0x20044438,0x48443844,0x4438783c,0x40780c34,0x106c4404,0x7c003838,0x7c380838,0x38203870,
    0x10002810,0x00340008,0x10100020,0x00100000,0x20040020,0x103c7000,0x1c007c1c,0x10700000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00380000,0x30000000,0x00004004,0x00000000,0x00000038,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
};

// A float out of the param block, indexed as a flat array. Written out as a
// select rather than `p[i >> 2][i & 3]`: a dynamic component index into a
// buffer element is the one place this driver returns the wrong lane (the text
// came out blank until this was explicit), and a select is three instructions.
float param_f(uint i)
{
    const vec4 v = p[i >> 2u];
    const uint c = i & 3u;
    return c == 0u ? v.x : (c == 1u ? v.y : (c == 2u ? v.z : v.w));
}

// ---- the two lookups microprofile's compute shader does ------------------
uint glyph_offset(int ch)
{
    ch &= 255;
    return bitfieldExtract(g_font_desc[ch >> 1], 16 - (ch & 1) * 16, 16);
}

uint glyph_bit(uint u, uint v)
{
    const uint t = u + 1024u * v;
    return bitfieldExtract(g_font_bits[t >> 5u], 31 - int(t & 31u), 1);
}

// 1.0 for a lit pixel, 0.45 for the drop shadow, 0 off the text.
// `base` is the first param vec4 of the characters, `maxchars` their count.
float text_px(vec2 px, vec2 org, float scale, uint base, uint maxchars, uint b0)
{
    const vec2 l = px - org;
    if (l.x < 0.0 || l.y < 0.0) return 0.0;

    const float cw = 6.0 * scale;                 // 5 columns + 1 of margin
    if (l.y >= 10.0 * scale) return 0.0;          // 8 rows + 1 shadow + margin
    const uint i = uint(l.x / cw);
    if (i >= maxchars) return 0.0;

    const int code = int(param_f((base + b0) * 4u + i) + 0.5);
    if (code <= 0) return 0.0;

    const float gx = (l.x - float(i) * cw) / scale;
    if (gx > 5.0) return 0.0;
    const float gy = l.y / scale;
    if (gy >= 9.0) return 0.0;          // the sheet is 9 rows: never index past it

    const uint u = glyph_offset(code) + uint(gx);
    if (glyph_bit(u, uint(gy)) != 0u) return 1.0;
    // row 8 is the drop shadow of row 7, exactly as microprofile draws it
    if (gy < 8.0 && glyph_bit(u, uint(gy) + 1u) != 0u) return 0.45;
    return 0.0;
}

float sd_box(vec2 c, vec2 half_size, float r)
{
    const vec2 q = abs(c) - half_size + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    const uint  b    = pc.const_off >> 4;
    const vec2  size = p[b + 0].zw;
    const vec2  px   = v_uv * size;
    const float u    = size.y * 0.25;
    const float ms   = p[b + 1].z;

    // ---- panel ------------------------------------------------------------
    const float sd  = sd_box(px - size * 0.5, size * 0.5, u * 0.32);
    float alpha     = (1.0 - smoothstep(-1.2, 0.8, sd)) * 0.74;
    vec3  col       = vec3(0.03, 0.035, 0.05);
    col += (1.0 - smoothstep(0.0, 1.4, -sd)) * vec3(0.10, 0.12, 0.16);

    // ---- the graph: 96 frame intervals, newest on the right ---------------
    const vec2 g0 = vec2(size.x * 0.46, u * 0.30);
    const vec2 gs = vec2(size.x * 0.54 - u * 0.45, size.y - u * 0.60);

    if (px.x > g0.x && px.x < g0.x + gs.x && px.y > g0.y && px.y < g0.y + gs.y) {
        const float gx = (px.x - g0.x) / gs.x;
        const float gy = (px.y - g0.y) / gs.y;
        const float fi = gx * NSAMP;
        const uint  i  = uint(clamp(fi, 0.0, NSAMP - 1.0));
        const float v  = clamp(param_f((b + G_LO) * 4u + i), 0.0, 1.0);

        const float pitch = gs.x / NSAMP;
        const float f     = fract(fi) * pitch;
        const float inbar = step(0.0, f - 0.6) * (1.0 - step(pitch - 0.4, f));
        const float top   = 1.0 - v;
        const float fill  = smoothstep(top - 0.02, top + 0.02, gy) * inbar;

        const vec3 bar = ms < 20.0 ? vec3(0.25, 0.92, 0.45)
                       : ms < 34.0 ? vec3(0.98, 0.78, 0.20)
                                   : vec3(0.98, 0.30, 0.26);
        col = mix(col, bar, fill * 0.95);

        // the 16.7 ms line: the only reference that matters at 60 Hz
        col = mix(col, vec3(0.35, 0.38, 0.45),
                  (1.0 - smoothstep(0.0, 1.2, abs(gy - 0.5) * gs.y)) * 0.75);
    }

    // ---- two lines of real text ------------------------------------------
    const float s1 = p[b + 1].x, s2 = p[b + 1].y;

    const float a1 = text_px(px, vec2(u * 0.40, size.y * 0.26), s1, 2u, 24u, b);
    const vec3  c1 = ms < 20.0 ? vec3(0.55, 1.00, 0.65)
                   : ms < 34.0 ? vec3(1.00, 0.88, 0.45)
                               : vec3(1.00, 0.45, 0.40);
    col = mix(col, c1, a1);

    const float a2 = text_px(px, vec2(u * 0.40, size.y * 0.26 + 10.0 * s1 + u * 0.20), s2, 8u, 24u, b);
    col = mix(col, vec3(0.72, 0.78, 0.86), a2);

    o_color = vec4(col, alpha);
}
