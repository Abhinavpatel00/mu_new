static void m4_identity(float *m) { memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1.0f; }

/* Column-major, like GLSL: element (row r, column c) lives at m[c * 4 + r].
 * That is the order m4_look() and m4_persp() write, and the order a mat4
 * uniform reads, so the product has to be taken in the same order too: out =
 * a * b means out(r,c) = sum_k a(r,k) * b(k,c) = sum_k a[k*4+r] * b[c*4+k]. */
static void m4_mul(float *out, const float *a, const float *b)
{
    float t[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                           a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(out, t, sizeof t);
}

/* Vulkan clip space: y down, depth 0..1. */
static void m4_persp(float *m, float fovy, float aspect, float zn, float zf)
{
    memset(m, 0, 16 * sizeof(float));
    const float t = 1.0f / tanf(fovy * 0.5f);
    m[0]  = t / aspect;
    m[5]  = -t;
    m[10] = zf / (zn - zf);
    m[11] = -1.0f;
    m[14] = zn * zf / (zn - zf);
}

static void m4_look(float *m, const float eye[3], const float at[3], const float upv[3])
{
    float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    f[0] /= l; f[1] /= l; f[2] /= l;

    float s[3] = { f[1] * upv[2] - f[2] * upv[1], f[2] * upv[0] - f[0] * upv[2], f[0] * upv[1] - f[1] * upv[0] };
    l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    s[0] /= l; s[1] /= l; s[2] /= l;

    const float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };

    m[0] = s[0];  m[1] = u[0];  m[2] = -f[0];  m[3]  = 0.0f;
    m[4] = s[1];  m[5] = u[1];  m[6] = -f[1];  m[7]  = 0.0f;
    m[8] = s[2];  m[9] = u[2];  m[10] = -f[2]; m[11] = 0.0f;
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] =  (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    m[15] = 1.0f;
}
