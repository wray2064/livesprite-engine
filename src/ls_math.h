#pragma once

// Deterministic trigonometry.
//
// The engine promises that the same document, engine version and profile
// produce the same bytes on every platform. Standard-library sin, cos, tan and
// atan2 cannot carry that promise: IEEE-754 does not specify them to the last
// bit, and MSVC's CRT, glibc, musl and Apple's libm genuinely disagree in the
// low bits. One bit of difference in a rotation angle moves a coverage
// decision, which moves a pixel, which breaks a golden hash on someone else's
// machine.
//
// So the engine carries its own. These are the fdlibm and Cephes kernels,
// evaluated in double and returned as float: accurate to well under a float
// ulp, and identical everywhere because they are made only of +, -, * and /,
// which IEEE-754 *does* specify exactly. The build pins -ffp-contract=off so
// the compiler cannot fuse those operations into an FMA behind our back.
//
// sqrt, floor, round, fabs and fmod are absent on purpose: IEEE-754 specifies
// all of them exactly, so the standard library versions are already
// deterministic and there is nothing to replace.

#include <cmath>
#include <cstdint>

namespace ls {
namespace math {

constexpr double kPiOver2   = 1.57079632679489661923;
constexpr double kPi        = 3.14159265358979323846;
constexpr double kTwoPi     = 6.28318530717958647692;
constexpr double kTwoOverPi = 0.63661977236758134308;

// pi/2 split into three doubles so that x - k*(pi/2) can be formed without
// losing the low bits of the reduction (Cody-Waite).
constexpr double kPiOver2Hi  = 1.57079632673412561417e+00;
constexpr double kPiOver2Mid = 6.07710050650619224932e-11;
constexpr double kPiOver2Lo  = 2.02226624879595063154e-21;

// sin on [-pi/4, pi/4], fdlibm __kernel_sin coefficients.
inline double kernelSin(double x) {
    const double z = x * x;
    const double r = -1.66666666666666324348e-01
        + z * (8.33333333332248946124e-03
        + z * (-1.98412698298579493134e-04
        + z * (2.75573137070700676789e-06
        + z * (-2.50507602534068634195e-08
        + z * 1.58969099521155010221e-10))));
    return x + x * z * r;
}

// cos on [-pi/4, pi/4], fdlibm __kernel_cos coefficients.
inline double kernelCos(double x) {
    const double z = x * x;
    const double r = 4.16666666666666019037e-02
        + z * (-1.38888888888741095749e-03
        + z * (2.48015872894767294178e-05
        + z * (-2.75573143513906633035e-07
        + z * (2.08757232129817482790e-09
        + z * -1.13596475577881948265e-11))));
    return 1.0 - 0.5 * z + z * z * r;
}

// Reduces x to [-pi/4, pi/4] and reports which quadrant it came from.
// Very large arguments are folded through fmod first: fmod is exact and
// deterministic, so the result stays reproducible even where it stops being
// accurate. Nothing in the engine feeds angles that big, but a driven
// parameter is under app control and must not be able to produce garbage that
// differs per platform.
inline double reduceQuadrant(double x, int& quadrant) {
    if (!(x == x) || x > 1e300 || x < -1e300) { quadrant = 0; return 0.0; }
    if (x > 1e9 || x < -1e9) { x = std::fmod(x, kTwoPi); }

    const double k = std::floor(x * kTwoOverPi + 0.5);
    quadrant = static_cast<int>(static_cast<int64_t>(k) & 3);
    return ((x - k * kPiOver2Hi) - k * kPiOver2Mid) - k * kPiOver2Lo;
}

inline double sinDouble(double x) {
    int quadrant = 0;
    const double r = reduceQuadrant(x, quadrant);
    switch (quadrant) {
        case 0:  return kernelSin(r);
        case 1:  return kernelCos(r);
        case 2:  return -kernelSin(r);
        default: return -kernelCos(r);
    }
}

inline double cosDouble(double x) {
    int quadrant = 0;
    const double r = reduceQuadrant(x, quadrant);
    switch (quadrant) {
        case 0:  return kernelCos(r);
        case 1:  return -kernelSin(r);
        case 2:  return -kernelCos(r);
        default: return kernelSin(r);
    }
}

// atan on the whole line, Cephes kernel. Reduces to [0, tan(pi/8)] by two
// range folds, then a rational minimax.
inline double atanDouble(double x) {
    if (!(x == x)) { return 0.0; }

    const bool negative = x < 0.0;
    if (negative) { x = -x; }

    double y = 0.0;
    if (x > 2.414213562373095) {          // tan(3*pi/8)
        y = kPiOver2;
        x = x > 1e300 ? 0.0 : -1.0 / x;
    } else if (x > 0.4142135623730950) {  // tan(pi/8)
        y = kPiOver2 * 0.5;
        x = (x - 1.0) / (x + 1.0);
    }

    const double z = x * x;
    const double p = ((((-8.750608600031904122785e-01 * z
        - 1.615753718733365076637e+01) * z
        - 7.500855792314704667340e+01) * z
        - 1.228866684490136173410e+02) * z
        - 6.485021904942025371773e+01);
    const double q = (((((z + 2.485846490142306297962e+01) * z
        + 1.650270098316988542046e+02) * z
        + 4.328810604912902668951e+02) * z
        + 4.853903996359136964868e+02) * z
        + 1.945506571482613964425e+02);

    y = y + x * z * p / q + x;
    return negative ? -y : y;
}

inline double atan2Double(double y, double x) {
    if (x == 0.0 && y == 0.0) { return 0.0; }
    if (x == 0.0)             { return y > 0.0 ? kPiOver2 : -kPiOver2; }

    const double a = atanDouble(y / x);
    if (x > 0.0) { return a; }
    return y >= 0.0 ? a + kPi : a - kPi;
}

// The float entry points the engine actually calls.
inline float sinf(float x)  { return static_cast<float>(sinDouble(static_cast<double>(x))); }
inline float cosf(float x)  { return static_cast<float>(cosDouble(static_cast<double>(x))); }
inline float atan2f(float y, float x) {
    return static_cast<float>(atan2Double(static_cast<double>(y), static_cast<double>(x)));
}

// tan as sin/cos, from the same reduction, so it agrees with them by
// construction. A vertical tangent saturates rather than returning infinity:
// the callers are skew angles, and an infinite shear factor would poison a
// transform matrix instead of merely producing a very steep one.
inline float tanf(float x) {
    const double d = static_cast<double>(x);
    const double s = sinDouble(d);
    const double c = cosDouble(d);
    if (c > -1e-12 && c < 1e-12) { return s >= 0.0 ? 1e12f : -1e12f; }
    return static_cast<float>(s / c);
}

}  // namespace math
}  // namespace ls
