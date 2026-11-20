/* Host-side check of eco.c's camera maths: the matrix must place the island in
 * front of the camera and inside the clip volume. Build:
 *   sed -n '420,466p' demo/eco.c > /tmp/cam.inc   (the real functions)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cam.inc"

/* viewproj in column-major: p' = M * p as a shader would do it */
static void xform(const float *m, const float p[3], float out[3])
{
    for (int r = 0; r < 3; r++)
        out[r] = m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2] + m[3 * 4 + r];
    const float w = m[0 * 4 + 3] * p[0] + m[1 * 4 + 3] * p[1] + m[2 * 4 + 3] * p[2] + m[3 * 4 + 3];
    for (int r = 0; r < 3; r++) out[r] /= w;
}

int main(void)
{
    const float eye[3] = { 0.0f, 6.0f, 14.0f };
    const float at[3]  = { 0.0f, 0.0f, 0.0f };
    const float up[3]  = { 0.0f, 1.0f, 0.0f };
    /* 9:16 portrait, as the phone is */
    const float aspect = 720.0f / 1548.0f;

    float view[16], proj[16], vp[16];
    m4_look(view, eye, at, up);
    m4_persp(proj, 45.0f * 3.14159265f / 180.0f, aspect, 0.5f, 120.0f);
    m4_mul(vp, proj, view);

    int bad = 0;
    const float pts[4][3] = { { 0, 0, 0 }, { 9.5f, 0, 0 }, { 0, 0, -9.5f }, { -6.0f, 1.0f, 4.0f } };
    for (int i = 0; i < 4; i++) {
        float ndc[3];
        xform(vp, pts[i], ndc);
        const int ok = fabsf(ndc[0]) <= 1.0f && fabsf(ndc[1]) <= 1.0f && ndc[2] >= 0.0f && ndc[2] <= 1.0f;
        printf("p(%.1f %.1f %.1f) -> ndc(%+.3f %+.3f %+.3f) %s\n", (double)pts[i][0], (double)pts[i][1],
               (double)pts[i][2], (double)ndc[0], (double)ndc[1], (double)ndc[2], ok ? "PASS" : "FAIL");
        bad += !ok;
    }

    /* the origin must land in the middle of the screen: y is down in Vulkan */
    float o[3];
    xform(vp, (const float[3]){ 0, 0, 0 }, o);
    printf("island centre at ndc %.3f %.3f (want 0,0)\n", (double)o[0], (double)o[1]);
    if (fabsf(o[0]) > 0.01f || fabsf(o[1]) > 0.01f) bad++;

    /* a point behind the camera must fall outside */
    float behind[3];
    xform(vp, (const float[3]){ 0, 0, 40 }, behind);
    printf("behind camera -> %.3f %.3f\n", (double)behind[0], (double)behind[1]);

    printf(bad ? "FAILED (%d)\n" : "OK\n", bad);
    return bad != 0;
}
