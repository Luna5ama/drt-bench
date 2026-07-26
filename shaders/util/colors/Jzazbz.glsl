/*
    References:
        [SAF17] Safdar, M., Cui, G., Kim, Y. J., & Luo, M. R. (2017).
            Perceptually uniform color space for image signals including high dynamic range and wide gamut.
            Optics Express, 25(13), 15131-15151.
            https://doi.org/10.1364/OE.25.015131
*/
#ifndef INCLUDE_util_colors_Jzazbz_glsl
#define INCLUDE_util_colors_Jzazbz_glsl a

#include "/util/Math.glsl"

// -------------------------------------------------- Constants --------------------------------------------------

// Jzazbz model parameters (Safdar 2017)
const float _JZAZBZ_B  = 1.15;
const float _JZAZBZ_G  = 0.66;
const float _JZAZBZ_D  = -0.56;
const float _JZAZBZ_D0 = 1.6295499532821566e-11;

// PQ constants (ST 2084) used internally by Jzazbz
const float _JZAZBZ_PQ_C1   = 3424.0 / 4096.0;
const float _JZAZBZ_PQ_C2   = 2413.0 / 128.0;
const float _JZAZBZ_PQ_C3   = 2392.0 / 128.0;
const float _JZAZBZ_PQ_N    = 2610.0 / 16384.0;
const float _JZAZBZ_PQ_P    = 1.7 * 2523.0 / 32.0;
const float _JZAZBZ_PQ_PEAK = 10000.0;

// -------------------------------------------------- Matrices --------------------------------------------------

// XYZ' -> LMS (Safdar 2017)
// GLSL mat3 is column-major: mat3(col0, col1, col2)
// Row-major from paper:
//   [ 0.41478972  0.579999   0.014648  ]
//   [-0.20151     1.120649   0.0531008 ]
//   [-0.0166008   0.264800   0.6684799 ]
const mat3 _JZAZBZ_XYZ_TO_LMS = mat3(
    0.41478972, -0.20151,   -0.0166008,
    0.579999,    1.120649,   0.264800,
    0.014648,    0.0531008,  0.6684799
);

// LMS -> modified XYZ' (inverse of above)
const mat3 _JZAZBZ_LMS_TO_XYZ = mat3(
    1.92422643578761,   0.350316762094999, -0.0909828109828476,
   -1.00479231259537,   0.726481193931655, -0.312728290523074,
    0.037651404030618, -0.065384422948085,  1.52276656130526
);

// L'M'S' -> Izazbz
const mat3 _JZAZBZ_PQ_TO_IZAZBZ = mat3(
    0.5,      3.524000,  0.199076,
    0.5,     -4.066708,  1.096799,
    0.0,      0.542708, -1.295875
);

// Izazbz -> L'M'S'
const mat3 _JZAZBZ_IZAZBZ_TO_PQ = mat3(
    1.0,  1.0,  1.0,
    0.138605043271539,  -0.138605043271539,  -0.0960192420263189,
    0.0580473161561189, -0.0580473161561189, -0.811891896056039
);

// -------------------------------------------------- PQ Transfer --------------------------------------------------

vec3 _jzazbz_pqForward(vec3 lms) {
    lms = softMin(lms, vec3(_JZAZBZ_PQ_PEAK));
    vec3 y = pow(lms / _JZAZBZ_PQ_PEAK, vec3(_JZAZBZ_PQ_N));
    return pow((_JZAZBZ_PQ_C1 + _JZAZBZ_PQ_C2 * y) / (1.0 + _JZAZBZ_PQ_C3 * y), vec3(_JZAZBZ_PQ_P));
}

vec3 _jzazbz_pqInverse(vec3 lmsPQ) {
    vec3 p = pow(max(lmsPQ, 0.0), vec3(1.0 / _JZAZBZ_PQ_P));
    vec3 denominator = max(_JZAZBZ_PQ_C2 - _JZAZBZ_PQ_C3 * p, 1e-8);
    return _JZAZBZ_PQ_PEAK * pow(max(p - _JZAZBZ_PQ_C1, 0.0) / denominator, vec3(1.0 / _JZAZBZ_PQ_N));
}

// -------------------------------------------------- XYZ <-> Jzazbz --------------------------------------------------

// CIE XYZ (D65, absolute nits) -> Jzazbz
vec3 jzazbz_fromXYZ(vec3 xyz) {
    // XYZ pre-modification
    float Xp = _JZAZBZ_B * xyz.x - (_JZAZBZ_B - 1.0) * xyz.z;
    float Yp = _JZAZBZ_G * xyz.y - (_JZAZBZ_G - 1.0) * xyz.x;
    float Zp = xyz.z;

    // XYZ' -> LMS
    vec3 lms = _JZAZBZ_XYZ_TO_LMS * vec3(Xp, Yp, Zp);

    // LMS -> L'M'S' (PQ forward)
    vec3 lmsPQ = _jzazbz_pqForward(max(lms, 0.0));

    // L'M'S' -> Izazbz
    vec3 izazbz = _JZAZBZ_PQ_TO_IZAZBZ * lmsPQ;
    float Iz = izazbz.x;

    // Iz -> Jz
    float Jz = (1.0 + _JZAZBZ_D) * Iz / (1.0 + _JZAZBZ_D * Iz) - _JZAZBZ_D0;

    return vec3(Jz, izazbz.y, izazbz.z);
}

// Jzazbz -> CIE XYZ (D65, absolute nits)
vec3 jzazbz_toXYZ(vec3 jab) {
    // Jz -> Iz
    float tmp = jab.x + _JZAZBZ_D0;
    float Iz = tmp / (1.0 + _JZAZBZ_D - _JZAZBZ_D * tmp);

    // Izazbz -> L'M'S'
    vec3 lmsPQ = _JZAZBZ_IZAZBZ_TO_PQ * vec3(Iz, jab.y, jab.z);

    // L'M'S' -> LMS (PQ inverse)
    vec3 lms = _jzazbz_pqInverse(lmsPQ);

    // LMS -> modified XYZ'
    vec3 XpYpZp = _JZAZBZ_LMS_TO_XYZ * lms;

    // Undo XYZ pre-modification
    float X = (XpYpZp.x + (_JZAZBZ_B - 1.0) * XpYpZp.z) / _JZAZBZ_B;
    float Y = (XpYpZp.y + (_JZAZBZ_G - 1.0) * X) / _JZAZBZ_G;
    float Z = XpYpZp.z;

    return vec3(X, Y, Z);
}

// -------------------------------------------------- JzCzhz (polar) --------------------------------------------------

// Jzazbz -> JzCzhz: vec3(Jz, Cz, hz) where hz is in radians
vec3 jzczhz_fromJzazbz(vec3 jab) {
    return vec3(jab.x, length(jab.yz), atan(jab.z, jab.y + 1e-8));
}

// JzCzhz -> Jzazbz: vec3(Jz, Cz, hz) -> vec3(Jz, az, bz)
vec3 jzazbz_fromJzCzhz(vec3 jch) {
    return vec3(jch.x, jch.y * cos(jch.z), jch.y * sin(jch.z));
}

// ------------------------------------------ Jz <-> Nits (achromatic axis) ------------------------------------------
// Used for domain bridging: converts perceptual Jz to/from linear luminance in cd/m^2.
// Exact on the achromatic axis (az=bz=0); used as a luminance proxy for tone mapping.

float jzazbz_JzToNits(float Jz) {
    float tmp = Jz + _JZAZBZ_D0;
    float Iz = tmp / (1.0 + _JZAZBZ_D - _JZAZBZ_D * tmp);
    float p = pow(max(Iz, 0.0), 1.0 / _JZAZBZ_PQ_P);
    return _JZAZBZ_PQ_PEAK * pow(
        max(p - _JZAZBZ_PQ_C1, 0.0) / (_JZAZBZ_PQ_C2 - _JZAZBZ_PQ_C3 * p),
        1.0 / _JZAZBZ_PQ_N);
}

float jzazbz_nitsToJz(float nits) {
    float y = pow(nits / _JZAZBZ_PQ_PEAK, _JZAZBZ_PQ_N);
    float Iz = pow(
        (_JZAZBZ_PQ_C1 + _JZAZBZ_PQ_C2 * y) / (1.0 + _JZAZBZ_PQ_C3 * y),
        _JZAZBZ_PQ_P);
    return (1.0 + _JZAZBZ_D) * Iz / (1.0 + _JZAZBZ_D * Iz) - _JZAZBZ_D0;
}

#endif
