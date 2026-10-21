// kestrel64-sdk — freestanding float math.
#include <k64/fmath.h>

// Reduces x to [-pi, pi] by subtracting the nearest multiple of 2 * pi.
static float wrap_pi(float x) {
  const float inv2pi = 0.159154943f;
  float k = k64_floorf(x * inv2pi + 0.5f);
  return x - k * (2.f * K64_PI);
}

float k64_sinf(float x) {
  x = wrap_pi(x);
  // fold to [-pi/2, pi/2]: sin(pi - x) = sin(x)
  if (x > K64_PI * 0.5f) x = K64_PI - x;
  else if (x < -K64_PI * 0.5f) x = -K64_PI - x;
  // Odd polynomial x - x^3/3! + x^5/5! - x^7/7! with adjusted coefficients.
  float x2 = x * x;
  return x * (1.f + x2 * (-0.16666654f + x2 * (0.0083321608f + x2 * -0.00019515296f)));
}

// cos(x) = sin(x + pi / 2)
float k64_cosf(float x) { return k64_sinf(x + K64_PI * 0.5f); }

void k64_mat4_identity(k64_mat4_t *m) {
  for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) m->m[c][r] = c == r ? 1.f : 0.f;
}

void k64_mat4_mul(k64_mat4_t *out, const k64_mat4_t *a, const k64_mat4_t *b) {
  // Temporary, so out may alias a or b. Element m[c][r] is row r of a dotted with column c of b.
  k64_mat4_t t;
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      t.m[c][r] = a->m[0][r] * b->m[c][0] + a->m[1][r] * b->m[c][1] +
                  a->m[2][r] * b->m[c][2] + a->m[3][r] * b->m[c][3];
  *out = t;
}

void k64_mat4_perspective(k64_mat4_t *m, float fovy, float aspect, float n, float f) {
  float half = fovy * 0.5f;
  // cot = 1 / tan(fovy / 2); aspect scales x only. m[2][3] puts -z into w (GL clip convention).
  float cot = k64_cosf(half) / k64_sinf(half);
  k64_mat4_identity(m);
  m->m[0][0] = cot / aspect;
  m->m[1][1] = cot;
  m->m[2][2] = (f + n) / (n - f);
  m->m[2][3] = -1.f;
  m->m[3][2] = 2.f * f * n / (n - f);
  m->m[3][3] = 0.f;
}

// Vector helpers for k64_mat4_lookat.
static k64_vec3_t v3sub(k64_vec3_t a, k64_vec3_t b) { return (k64_vec3_t){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static k64_vec3_t v3cross(k64_vec3_t a, k64_vec3_t b) {
  return (k64_vec3_t){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
static float v3dot(k64_vec3_t a, k64_vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// Unit vector; a zero-length input gives the zero vector instead of a division by zero.
static k64_vec3_t v3norm(k64_vec3_t a) {
  float l = k64_sqrtf(v3dot(a, a));
  float il = l > 0 ? 1.f / l : 0.f;
  return (k64_vec3_t){ a.x * il, a.y * il, a.z * il };
}

void k64_mat4_lookat(k64_mat4_t *m, k64_vec3_t eye, k64_vec3_t at, k64_vec3_t up) {
  k64_vec3_t f = v3norm(v3sub(at, eye));
  k64_vec3_t s = v3norm(v3cross(f, up));
  k64_vec3_t u = v3cross(s, f);
  k64_mat4_identity(m);
  // Rows of the view matrix are s, u and -f; the translation is -dot(axis, eye).
  m->m[0][0] = s.x; m->m[1][0] = s.y; m->m[2][0] = s.z;
  m->m[0][1] = u.x; m->m[1][1] = u.y; m->m[2][1] = u.z;
  m->m[0][2] = -f.x; m->m[1][2] = -f.y; m->m[2][2] = -f.z;
  m->m[3][0] = -v3dot(s, eye);
  m->m[3][1] = -v3dot(u, eye);
  m->m[3][2] = v3dot(f, eye);
}

void k64_mat4_rotate_y(k64_mat4_t *m, float a) {
  float c = k64_cosf(a), s = k64_sinf(a);
  k64_mat4_identity(m);
  m->m[0][0] = c; m->m[2][0] = s; m->m[0][2] = -s; m->m[2][2] = c;
}

void k64_mat4_rotate_x(k64_mat4_t *m, float a) {
  float c = k64_cosf(a), s = k64_sinf(a);
  k64_mat4_identity(m);
  m->m[1][1] = c; m->m[2][1] = -s; m->m[1][2] = s; m->m[2][2] = c;
}

void k64_mat4_rotate_z(k64_mat4_t *m, float a) {
  float c = k64_cosf(a), s = k64_sinf(a);
  k64_mat4_identity(m);
  m->m[0][0] = c; m->m[1][0] = -s; m->m[0][1] = s; m->m[1][1] = c;
}

void k64_mat4_translate(k64_mat4_t *m, float x, float y, float z) {
  k64_mat4_identity(m);
  m->m[3][0] = x; m->m[3][1] = y; m->m[3][2] = z;
}

void k64_mat4_scale(k64_mat4_t *m, float x, float y, float z) {
  k64_mat4_identity(m);
  m->m[0][0] = x; m->m[1][1] = y; m->m[2][2] = z;
}
