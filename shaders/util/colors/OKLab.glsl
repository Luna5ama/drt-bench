/*
    References:
        [OTT20] Ottosson, Björn. "A perceptual color space for image processing". 2020.
            https://bottosson.github.io/posts/oklab/

    OKLab operates in relative luminance (1.0 = reference white).
    All XYZ values are D65-adapted, relative (not absolute nits).
*/
#ifndef INCLUDE_util_colors_OKLab_glsl
#define INCLUDE_util_colors_OKLab_glsl a

// -------------------------------------------------- Matrices --------------------------------------------------

// CIE XYZ (D65) -> LMS (OKLab cone response)
// GLSL mat3 = column-major
const mat3 _OKLAB_XYZ_TO_LMS = mat3(
    0.8189330101, 0.0329845436, 0.0482003018,
    0.3618667424, 0.9293118715, 0.2643662691,
    -0.1288597137, 0.0361456387, 0.6338517070
);

// LMS -> CIE XYZ (D65)
const mat3 _OKLAB_LMS_TO_XYZ = mat3(
    1.2270138511, -0.0405801784, -0.0763812845,
    -0.5577999807, 1.1122568696, -0.4214819784,
    0.2812561490, -0.0716766787, 1.5861632204
);

// cbrt(LMS) -> OKLab (L, a, b)
const mat3 _OKLAB_CRTLMS_TO_LAB = mat3(
    0.2104542553, 1.9779984951, 0.0259040371,
    0.7936177850, -2.4285922050, 0.7827717662,
    -0.0040720468, 0.4505937099, -0.8086757660
);

// OKLab (L, a, b) -> cbrt(LMS)
const mat3 _OKLAB_LAB_TO_CRTLMS = mat3(
    1.0, 1.0, 1.0,
    0.3963377774, -0.1055613458, -0.0894841775,
    0.2158037573, -0.0638541728, -1.2914855480
);

// -------------------------------------------------- XYZ <-> OKLab --------------------------------------------------

// CIE XYZ (D65, relative) -> OKLab
vec3 oklab_fromXYZ(vec3 xyz) {
    vec3 lms = _OKLAB_XYZ_TO_LMS * xyz;
    vec3 crtLms = pow(max(lms, 0.0), vec3(1.0 / 3.0));
    return _OKLAB_CRTLMS_TO_LAB * crtLms;
}

// OKLab -> CIE XYZ (D65, relative)
vec3 oklab_toXYZ(vec3 lab) {
    vec3 crtLms = _OKLAB_LAB_TO_CRTLMS * lab;
    vec3 lms = crtLms * crtLms * crtLms;
    return _OKLAB_LMS_TO_XYZ * lms;
}

// -------------------------------------------------- OKLCh (polar) --------------------------------------------------

// OKLab -> OKLCh: vec3(L, C, h) where h is in radians
vec3 oklch_fromOKLab(vec3 lab) {
    return vec3(lab.x, length(lab.yz), atan(lab.z, lab.y));
}

// OKLCh -> OKLab: vec3(L, C, h) -> vec3(L, a, b)
vec3 oklab_fromOKLCh(vec3 lch) {
    return vec3(lch.x, lch.y * cos(lch.z), lch.y * sin(lch.z));
}

#endif
