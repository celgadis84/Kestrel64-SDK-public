// kestrel64-sdk — freestanding float math for the VR4300 FPU (no libm).
//
// Notes on cost (VR4300): add/mul.s ~3-5 cycles, div.s ~29, sqrt.s ~29. trunc.w.s on an
// out-of-range value raises an *unmaskable* unimplemented-operation exception, so every
// float->int conversion here clamps first.
#pragma once
#include "hw.h"

#define K64_PI 3.14159265358979f

// sqrt.s (~29 cycles). x must be >= 0.
K64_INLINE float k64_sqrtf(float x) { float r; __asm__("sqrt.s %0, %1" : "=f"(r) : "f"(x)); return r; }
K64_INLINE float k64_fabsf(float x) { return x < 0 ? -x : x; }
K64_INLINE float k64_clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

// float -> s32 with saturation (safe trunc.w.s).
K64_INLINE s32 k64_f2i_sat(float f) {
  if (f >= 2147483520.f) return 0x7FFFFFFF;
  if (f <= -2147483648.f) return (s32)0x80000000;
  return (s32)f;
}
// Largest integer <= f, saturated to the s32 range.
K64_INLINE s32 k64_floori(float f) {
  s32 i = k64_f2i_sat(f);
  return (f < (float)i) ? i - 1 : i;
}
// floor() for floats; |f| >= 2^23 is already integral and returned as is.
K64_INLINE float k64_floorf(float f) {
  if (f >= 8388608.f || f <= -8388608.f) return f;   // already integral
  return (float)k64_floori(f);
}
// float -> s15.16 fixed, floor-rounded, saturated (RDP edge/attribute format).
K64_INLINE s32 k64_f2fx16(float f) {
  if (f >= 32767.99f) return 0x7FFFFFFF;
  if (f < -32768.f) return (s32)0x80000000;
  return k64_floori(f * 65536.f);
}

// sin/cos of radians: range-reduce to [-pi/2, pi/2], then a 7th order odd minimax polynomial
// (~1e-4 absolute error). cos(x) = sin(x + pi/2). No divides.
float k64_sinf(float x);
float k64_cosf(float x);

// ---- 4x4 matrices, column-major (m[col][row]), column vectors: v' = M v --------------------
// Right-handed, OpenGL conventions: the camera looks down -Z, clip z is -w..w. Every builder
// below overwrites *m (no accumulation); combine with k64_mat4_mul. Rotations are radians,
// counterclockwise looking from +axis toward the origin.
typedef struct { float m[4][4]; } k64_mat4_t;
typedef struct { float x, y, z; } k64_vec3_t;

void k64_mat4_identity(k64_mat4_t *m);
void k64_mat4_mul(k64_mat4_t *out, const k64_mat4_t *a, const k64_mat4_t *b);   // out = a*b (out may alias)
// gluPerspective: fovy_rad = full vertical angle, aspect = width / height, 0 < znear < zfar.
void k64_mat4_perspective(k64_mat4_t *m, float fovy_rad, float aspect, float znear, float zfar);
// gluLookAt: world -> eye space; up need not be unit or orthogonal, only not parallel to
// at - eye.
void k64_mat4_lookat(k64_mat4_t *m, k64_vec3_t eye, k64_vec3_t at, k64_vec3_t up);
void k64_mat4_rotate_y(k64_mat4_t *m, float a);
void k64_mat4_rotate_x(k64_mat4_t *m, float a);
void k64_mat4_rotate_z(k64_mat4_t *m, float a);
void k64_mat4_translate(k64_mat4_t *m, float x, float y, float z);
void k64_mat4_scale(k64_mat4_t *m, float x, float y, float z);
