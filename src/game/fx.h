#pragma once

#include <nds/arm9/math.h>
#include <nds/ndstypes.h>
#include <stdint.h>

// Fixed point for the vehicle physics (the ARM9 has no FPU: a soft-float
// multiply / add is ~40 cycles, sqrtf / sinf hundreds to thousands).
//  - q   = Q8.24 in an s32 (+-128, resolution 6e-8): velocities per 1/50 s
//          step, turn speeds, unit axes, model-space offsets, coefficients.
//          The flying handling has terms near 1e-5 per step, so Q16 would
//          lose them.
//  - wp  = Q24 in an s64: world positions (+-5e11 m), only added to / and
//          subtracted from (relative vectors are q).
// Products go through int64 (one smull), divisions and square roots through
// the hardware divider / sqrt unit (not re-entrant: never from an IRQ).
namespace fx {

using q = int32_t;
using wp = int64_t;
constexpr int SH = 24;
constexpr q ONE = 1 << SH;

constexpr q F(double x) { return (q)(x * 16777216.0 + (x < 0 ? -0.5 : 0.5)); }
inline q fromFloat(float x) { return (q)(x * 16777216.0f); }
inline float toFloat(q x) { return (float)x * (1.0f / 16777216.0f); }
inline q fromInt(int x) { return (q)(x << SH); }

inline q mul(q a, q b) { return (q)(((int64_t)a * b) >> SH); }
inline q mul3(q a, q b, q c) { return mul(mul(a, b), c); }
inline q abs(q a) { return a < 0 ? -a : a; }
inline q min(q a, q b) { return a < b ? a : b; }
inline q max(q a, q b) { return a > b ? a : b; }
inline q clamp(q v, q lo, q hi) { return v < lo ? lo : v > hi ? hi : v; }

// a / b (both q). Saturates instead of wrapping when |a/b| >= 128.
inline q div(q a, q b) {
	if (!b) return a >= 0 ? INT32_MAX : -INT32_MAX;
	const int64_t n = (int64_t)a << SH;
	const int64_t lim = (int64_t)(b < 0 ? -(int64_t)b : b) << 31;
	if (n >= lim || -n >= lim) return (a < 0) != (b < 0) ? -INT32_MAX : INT32_MAX;
	return div64(n, b);
}
// 1 / b
inline q inv(q b) { return div(ONE, b); }

// sqrt of a q (a >= 0); sqrt of a Q48 int64 (dot products before the shift)
inline q sqrt(q a) { return a <= 0 ? 0 : (q)sqrt64((int64_t)a << SH); }
inline q sqrt48(int64_t a) { return a <= 0 ? 0 : (q)sqrt64(a); }

// sin / cos: Taylor to x^7 inside +-pi/2 (steer angles, flight attitudes),
// libnds' table for anything larger. Radians in q.
q sin(q a);
q cos(q a);
// CMaths::ASin-alike for |x| <= 1 (Abramowitz & Stegun 4.4.45, 5e-5).
q asin(q x);
// ln / exp for the per-step damping powers (pow(x, t) = exp(t ln x)).
q ln(q x);       // x > 0
q exp(q x);      // x <= ~4.8

struct V {
	q x, y, z;
	q& operator[](int i) { return (&x)[i]; }
	q operator[](int i) const { return (&x)[i]; }
};
inline V operator+(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V operator-(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V operator-(V a) { return { -a.x, -a.y, -a.z }; }
inline V& operator+=(V& a, V b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline V& operator-=(V& a, V b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
inline V operator*(V a, q s) { return { mul(a.x, s), mul(a.y, s), mul(a.z, s) }; }
inline V& operator*=(V& a, q s) { a = a * s; return a; }
// Q48 dot (no rounding before the caller's shift / sqrt)
inline int64_t dot48(V a, V b) { return (int64_t)a.x * b.x + (int64_t)a.y * b.y + (int64_t)a.z * b.z; }
inline q dot(V a, V b) { return (q)(dot48(a, b) >> SH); }
inline V cross(V a, V b) {
	return { (q)(((int64_t)a.y * b.z - (int64_t)a.z * b.y) >> SH), (q)(((int64_t)a.z * b.x - (int64_t)a.x * b.z) >> SH),
	         (q)(((int64_t)a.x * b.y - (int64_t)a.y * b.x) >> SH) };
}
inline q len(V a) { return sqrt48(dot48(a, a)); }
inline q len2(V a) { return dot(a, a); }
// in place, returns the old length (0 = left untouched)
q normalise(V& a);
inline V madd(V a, V b, q s) { return a + b * s; }

inline V fromFloat3(const float* f) { return { fromFloat(f[0]), fromFloat(f[1]), fromFloat(f[2]) }; }
inline void toFloat3(V a, float* f) { f[0] = toFloat(a.x); f[1] = toFloat(a.y); f[2] = toFloat(a.z); }

}
