/*
    References:
        [DLB17] Dolby Laboratories. "ICtCp". 2017.
            Part of ITU-R BT.2100 / BT.2124.
        [ITU23] ITU-R BT.2100-2 (2023). Image parameter values for HDR television.

    ICtCp operates in absolute luminance (nits) via PQ encoding.
    XYZ values are D65-adapted, absolute nits.
*/
#ifndef INCLUDE_util_colors_ICtCp_glsl
#define INCLUDE_util_colors_ICtCp_glsl a

#include "../Colors2.glsl"

// -------------------------------------------------- Matrices --------------------------------------------------

// CIE XYZ (D65) -> LMS (ICtCp, BT.2124 crosstalk matrix)
// GLSL mat3 = column-major
const mat3 _ICTCP_XYZ_TO_LMS = mat3(
    0.3592832590, -0.1920808463, 0.0070797844,
    0.6976051147, 1.1004767970, 0.0748396662,
    -0.0358915361, 0.0753748827, 0.8433265453
);

// LMS -> CIE XYZ (D65)
const mat3 _ICTCP_LMS_TO_XYZ = mat3(
    2.0701522183, 0.3647385209, -0.0497472075,
    -1.3263473389, 0.6805660249, -0.0492609666,
    0.2066510476, -0.0453045458, 1.1880659249
);

// PQ-encoded LMS -> ICtCp
const mat3 _ICTCP_PQLMS_TO_ICTCP = mat3(
    0.5, 1.6137695313, 4.3781738281,
    0.5, -3.3234863281, -4.2456054688,
    0.0, 1.7097167969, -0.1325683594
);

// ICtCp -> PQ-encoded LMS
const mat3 _ICTCP_ICTCP_TO_PQLMS = mat3(
    1.0, 1.0, 1.0,
    0.008609037037932, -0.008609037037932, 0.560031335710679,
    0.111029625003026, -0.111029625003026, -0.320627174987319
);

// -------------------------------------------------- XYZ <-> ICtCp --------------------------------------------------

// CIE XYZ (D65, absolute nits) -> ICtCp
vec3 ictcp_fromXYZ(vec3 xyz) {
    vec3 lms = _ICTCP_XYZ_TO_LMS * xyz;
    vec3 pqLms = colors2_oetf_PQ(max(lms, 0.0));
    return _ICTCP_PQLMS_TO_ICTCP * pqLms;
}

// ICtCp -> CIE XYZ (D65, absolute nits)
vec3 ictcp_toXYZ(vec3 ict) {
    vec3 pqLms = _ICTCP_ICTCP_TO_PQLMS * ict;
    vec3 lms = colors2_eotf_PQ(pqLms);
    return _ICTCP_LMS_TO_XYZ * lms;
}

// -------------------------------------------------- Polar form --------------------------------------------------

// ICtCp -> polar: vec3(I, C, h) where h is in radians
vec3 ictcp_toPolar(vec3 ict) {
    return vec3(ict.x, length(ict.yz), atan(ict.z, ict.y));
}

// polar -> ICtCp: vec3(I, C, h) -> vec3(I, Ct, Cp)
vec3 ictcp_fromPolar(vec3 ich) {
    return vec3(ich.x, ich.y * cos(ich.z), ich.y * sin(ich.z));
}

#endif
