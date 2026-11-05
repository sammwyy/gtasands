#include "game/fx.h"

namespace fx {

static constexpr q PI = F(3.14159265358979), HALF_PI = F(1.57079632679490), TWO_PI = F(6.28318530717959);
static constexpr q LN2 = F(0.693147180559945);

// |x| <= pi/2
static q sinCore(q x) {
	const q x2 = mul(x, x);
	q s = ONE - mul(x2, F(1.0 / 72.0));
	s = ONE - mul(mul(x2, F(1.0 / 42.0)), s);
	s = ONE - mul(mul(x2, F(1.0 / 20.0)), s);
	s = ONE - mul(mul(x2, F(1.0 / 6.0)), s);
	return mul(x, s);
}
static q cosCore(q x) {
	const q x2 = mul(x, x);
	q c = ONE - mul(x2, F(1.0 / 90.0));
	c = ONE - mul(mul(x2, F(1.0 / 56.0)), c);
	c = ONE - mul(mul(x2, F(1.0 / 30.0)), c);
	c = ONE - mul(mul(x2, F(1.0 / 12.0)), c);
	return ONE - mul(mul(x2, F(0.5)), c);
}

// -> (-pi, pi]
static q wrap(q a) {
	while (a > PI) a -= TWO_PI;
	while (a <= -PI) a += TWO_PI;
	return a;
}

q sin(q a) {
	if (a < -HALF_PI || a > HALF_PI) {
		a = wrap(a);
		if (a > HALF_PI) a = PI - a;
		else if (a < -HALF_PI) a = -PI - a;
	}
	return sinCore(a);
}

q cos(q a) {
	if (a < -HALF_PI || a > HALF_PI) {
		a = wrap(a);
		if (a > HALF_PI) return -cosCore(PI - a);
		if (a < -HALF_PI) return -cosCore(-PI - a);
	}
	return cosCore(a);
}

q asin(q x) {
	const bool neg = x < 0;
	if (neg) x = -x;
	if (x > ONE) x = ONE;
	q p = F(-0.0187293);
	p = mul(p, x) + F(0.0742610);
	p = mul(p, x) - F(0.2121144);
	p = mul(p, x) + F(1.5707288);
	const q r = HALF_PI - mul(sqrt(ONE - x), p);
	return neg ? -r : r;
}

q ln(q x) {
	if (x <= 0) return -INT32_MAX;
	const int k = 31 - __builtin_clz((uint32_t)x) - SH;   // x = m * 2^k, m in [1, 2)
	const q m = k >= 0 ? x >> k : x << -k;
	const q t = div(m - ONE, m + ONE), t2 = mul(t, t);
	q s = mul(t2, F(1.0 / 9.0)) + F(1.0 / 7.0);
	s = mul(s, t2) + F(1.0 / 5.0);
	s = mul(s, t2) + F(1.0 / 3.0);
	s = mul(s, t2) + ONE;
	return 2 * mul(t, s) + k * LN2;
}

q exp(q x) {
	if (x > F(4.85)) return INT32_MAX;
	if (x < F(-17.0)) return 0;
	int k = 0;
	while (x >= LN2) { x -= LN2; k++; }
	while (x < 0) { x += LN2; k--; }
	q e = ONE + mul(x, F(1.0 / 7.0));
	e = ONE + mul(mul(x, F(1.0 / 6.0)), e);
	e = ONE + mul(mul(x, F(1.0 / 5.0)), e);
	e = ONE + mul(mul(x, F(1.0 / 4.0)), e);
	e = ONE + mul(mul(x, F(1.0 / 3.0)), e);
	e = ONE + mul(mul(x, F(1.0 / 2.0)), e);
	e = ONE + mul(x, e);
	return k >= 0 ? e << k : e >> -k;
}

q normalise(V& a) {
	const q l = len(a);
	if (l <= 16) return 0;
	// per component: 1 / l overflows q for the short force vectors (|v| < 1/128)
	for (int k = 0; k < 3; k++) a[k] = div64((int64_t)a[k] << SH, l);
	return l;
}

}
