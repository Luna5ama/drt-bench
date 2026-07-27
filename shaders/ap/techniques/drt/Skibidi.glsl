#ifndef INCLUDE_techniques_drt_Skibidi_glsl
#define INCLUDE_techniques_drt_Skibidi_glsl a

#include "Common.glsl"
#include "../../util/colors/Jzazbz.glsl"
#include "../../util/colors/OKLab.glsl"
#include "../../util/colors/ICtCp.glsl"

// -------------------------------------------- Absolute Luminance Anchor --------------------------------------------

// Reference white per ITU-R BT.2408: 203 cd/m^2 maps scene white (1.0) to a
// well-characterized point on the PQ curve, giving good fp32 precision.
const float _DRT_REFERENCE_WHITE_NITS = 203.0;
const float _DRT_DISPLAY_PEAK_NITS = 203.0;

// -------------------------------------------- UCS Abstraction Layer ------------------------------------------------
// Each UCS provides:
//   _drt_ucs_fromXYZ(xyz_rel)            -> vec3(L, C, h)   polar form
//   _drt_ucs_polarToXYZ(L, C, h)         -> vec3             relative XYZ
//   _drt_ucs_LToNits(L)                  -> float            lightness to nits
//   _drt_ucs_nitsToL(nits)               -> float            nits to lightness

//#if SETTING_DRT_UCS == 0
// ---- Jzazbz (PQ-based, HDR perceptual) ----

vec3 _drt_ucs_fromXYZ(vec3 xyz_rel) {
    vec3 jab = jzazbz_fromXYZ(xyz_rel * _DRT_REFERENCE_WHITE_NITS);
    return jzczhz_fromJzazbz(jab);
}

vec3 _drt_ucs_polarToXYZ(float L, float C, float h) {
    vec3 jab = jzazbz_fromJzCzhz(vec3(L, C, h));
    return jzazbz_toXYZ(jab) / _DRT_REFERENCE_WHITE_NITS;
}

vec3 _drt_ucs_XYZToPolar(vec3 xyz) {
    vec3 jab = jzazbz_fromXYZ(xyz * _DRT_REFERENCE_WHITE_NITS);
    return jzczhz_fromJzazbz(jab);
}

float _drt_ucs_LToNits(float L) { return jzazbz_JzToNits(L); }
float _drt_ucs_nitsToL(float nits) { return jzazbz_nitsToJz(nits); }

//#elif SETTING_DRT_UCS == 1
//// ---- OKLab (cube-root, SDR perceptual) ----
//// OKLab L ≈ cbrt(Y_relative). Nits bridge: L^3 * refWhite <-> cbrt(nits/refWhite).
//
//vec3 _drt_ucs_fromXYZ(vec3 xyz_rel) {
//    vec3 lab = oklab_fromXYZ(xyz_rel);
//    return oklch_fromOKLab(lab);
//}
//
//vec3 _drt_ucs_polarToXYZ(float L, float C, float h) {
//    vec3 lab = oklab_fromOKLCh(vec3(L, C, h));
//    return oklab_toXYZ(lab);
//}
//
//vec3 _drt_ucs_XYZToPolar(vec3 xyz) {
//    vec3 lab = oklab_fromXYZ(xyz);
//    return oklch_fromOKLab(lab);
//}
//
//float _drt_ucs_LToNits(float L) { return L * L * L * _DRT_REFERENCE_WHITE_NITS; }
//float _drt_ucs_nitsToL(float nits) { return pow(nits / _DRT_REFERENCE_WHITE_NITS, 1.0 / 3.0); }
//
//#elif SETTING_DRT_UCS == 2
//// ---- ICtCp (PQ-based, HDR perceptual, Dolby) ----
//// I ≈ PQ(nits) on the achromatic axis (D65 LMS ≈ nits).
//
//vec3 _drt_ucs_fromXYZ(vec3 xyz_rel) {
//    vec3 ict = ictcp_fromXYZ(xyz_rel * _DRT_REFERENCE_WHITE_NITS);
//    return ictcp_toPolar(ict);
//}
//
//vec3 _drt_ucs_polarToXYZ(float L, float C, float h) {
//    vec3 ict = ictcp_fromPolar(vec3(L, C, h));
//    return ictcp_toXYZ(ict) / _DRT_REFERENCE_WHITE_NITS;
//}
//
//vec3 _drt_ucs_XYZToPolar(vec3 xyz) {
//    vec3 ict = ictcp_fromXYZ(xyz * _DRT_REFERENCE_WHITE_NITS);
//    return ictcp_toPolar(ict);
//}
//
//float _drt_ucs_LToNits(float I) { return colors2_eotf_PQ(I); }
//float _drt_ucs_nitsToL(float nits) { return colors2_oetf_PQ(nits); }
//
//#endif

// https://www.desmos.com/calculator/sag7bctw6l
float coolSigmoid(float x) {
    const float m = SETTING_SKIBIDI_LINEAR_SLOPE;
    const float w = SETTING_SKIBIDI_LINEAR_LENGTH;
    const float x1 = SETTING_SKIBIDI_LINEAR_STARTX;
    const float y1 = SETTING_SKIBIDI_LINEAR_STARTY;
    const float ft = SETTING_SKIBIDI_TOE_SLOPE;
    const float fs = SETTING_SKIBIDI_SHOULDER_SLOPE;

    float l = m * (x - x1) + y1;
    float x2 = x1 + w;
    float y2 = m * (x2 - x1) + y1;
    float ys1 = max(y1, 0.00001);
    float ka = ys1 / x1;
    float bt = min(ft, 0.95 * ka);
    float nt = m - bt;
    float dt = ys1 - bt * x1;
    float ht = (x1 * nt) / max(dt, 0.00001);
    float at = dt / pow(x1, ht);
    float ftoe = max(at * pow(x, ht) + bt * x, 0.0);
    float u2 = 1.0 - x2;
    float v2 = 1.0 - y2;
    float ys2 = max(v2, 0.00001);
    float ks = ys2 / u2;
    float bs = min(fs, 0.95 * ks);
    float ns = m - bs;
    float ds = ys2 - bs * u2;
    float hs = (u2 * ns) / max(ds, 0.00001);
    float as = ds / pow(u2, hs);
    float fshd = 1.0 - (as * pow(1.0 - x, hs) + bs * (1.0 - x));

    return mix(ftoe, mix(l, fshd, x > x2), x > x1);
}

float applyCurve(float x) {
    const float min_ev = -16.0;
    const float max_ev = 16.0;
    float compressed = coolSigmoid(linearStep(min_ev, max_ev, log2(x)));

    return compressed;
}

// -------------------------------------------- Luminance Compression ------------------------------------------------
// Bridges UCS lightness to AgX's native log2 scene-linear domain.
// Steps: L -> nits -> scene-relative -> log2 -> AgX sigmoid -> display nits -> L

vec2 _drt_compressL(float L) {
    float nits = _drt_ucs_LToNits(L);
    float relative = nits / _DRT_REFERENCE_WHITE_NITS;
    float compressed = applyCurve(relative);
    return vec2(_drt_ucs_nitsToL(compressed * _DRT_DISPLAY_PEAK_NITS), compressed);
}

// -------------------------------------------- Gamut Boundary (Bisection) ------------------------------------------

bool _drt_isInGamut(vec3 rgb) {
    const vec3 lumaNorm = vec3(0.3396048918, 0.1009507830, 1.0); // TODO: Derive from output colorspace primaries instead of hardcoding sRGB-based values
    rgb *= lumaNorm; // Normalize so it doesn't compress the hell out of red/green gamut boundary due to their higher luma contribution
    return mmin3(rgb) >= -1e-5 && mmax3(rgb) <= 1.0 + 1e-5;
}

vec3 _drt_lchToOutputRGB(float L, float C, float h) {
    vec3 xyz = _drt_ucs_polarToXYZ(L, C, h);
    return colors2_colorspaces_convert(COLORS2_COLORSPACES_CIE_XYZ, COLORS2_OUTPUT_COLORSPACE, xyz);
}

float _drt_findBoundaryChroma(float L, float h) {
    float lo = 0.0;
    float hi = 0.5;
    L = max(L, 0.05);
    for (int i = 0; i < 8; ++i) {
        float mid = (lo + hi) * 0.5;
        if (_drt_isInGamut(_drt_lchToOutputRGB(L, mid, h))) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// -------------------------------------------- Chroma Compression --------------------------------------------------

float _drt_compressChroma(float C, float C_limit, float compressFactor) {
    if (C_limit <= 0.0) return 0.0;
    return C * pow(1.0 + pow(C / C_limit, compressFactor), -rcp(compressFactor));
}

// -------------------------------------------- Main DRT Entry Point -------------------------------------------------

float getHueDistance(float a, float b) {
    float diff = mod(a - b + PI, PI_2) - PI;
    return abs(diff);
}

float shortestPathLerp(float a, float b, float t) {
    // 1. 计算原始差异
    float diff = b - a;

    // 2. 将差异归一化到 [-PI, PI] 区间
    // GLSL 的 mod(x, y) 表现为 x - y * floor(x/y)，能完美处理负数
    float shortestDiff = mod(diff + PI, PI_2) - PI;

    // 3. 执行线性插值
    float result = a + shortestDiff * t;

    return result;
}

vec3 drt_skibidi(vec3 color) {
    vec3 xyz = color.rgb;
    xyz = max(xyz, 0.0);

    // XYZ -> UCS polar (L, C, h)
    vec3 lch = _drt_ucs_fromXYZ(xyz);
    float L = lch.x;
    float C = lch.y;
    float h = lch.z;

    // Luminance compression (AgX sigmoid via UCS lightness)
    vec2 compressTemp = _drt_compressL(L);
    float L_compressed = compressTemp.x;
    float L_scale = compressTemp.y;

    // Bezold–Brücke shift
    // Based on No-Contrast shift data on Pridmore 10.1016/s0042-6989(99)00085-1
    // Table 1, 6500 K | 10:100 (1:10 ratio)
    float BLUE_HUE = 3.8779017899854153; // 481 nm
    float GREEN_HUE = 2.9828771910822991; // 506 nm
//    float YELLOW_HUE = 1.5576802370459031; // 578 nm
    float YELLOW_HUE = 1.655501; // 575 nm

    // Fig. 4, 10:100 cd/m^2
    // float CYAN_HUE = 2.8842147926763664; // 510 nm
     float CYAN_HUE = 3.495110; // 490 nm, 510 was too green
    //float LIME_HUE = 1.809594129026481; // 570 nm
    float LIME_HUE = 2.181382; // 555 nm
    //    float RED_HUE = 0.59790626253265278; // 650 nm
    // float RED_HUE = 0.7256962303511727; // 620 nm, accurate number was 650 but that shifts pink/magenta too much
    float RED_HUE = 0.673878; // 640 nm, 620 was too orange
    //    float VIOLET_HUE = 4.7791986716767365; // 440 nm
    float VIOLET_HUE = 5.1; // Use this instead because purple/violet are non-sense

    float hueDistB = getHueDistance(h, BLUE_HUE);
    float hueDistG = getHueDistance(h, GREEN_HUE);
    float hueDistY = getHueDistance(h, YELLOW_HUE);

    float hueDistC = getHueDistance(h, CYAN_HUE);
    float hueDistL = getHueDistance(h, LIME_HUE);
    float hueDistR = getHueDistance(h, RED_HUE);
    float hueDistV = getHueDistance(h, VIOLET_HUE);

    float C_max1 = _drt_findBoundaryChroma(L_compressed, h);

    float shiftChromaFactor = saturate(C * safeRcp(C_max1));
    float shiftLFactor = sqrt(smoothstep(0.0, 0.4, pow2(L))) * shiftChromaFactor;

    float hShifted = h;
    hShifted = shortestPathLerp(hShifted, GREEN_HUE, exp2(-4.8 * pow2(hueDistC)) * shiftLFactor * SETTING_SKIBIDI_BB_SHIFT_C);
    hShifted = shortestPathLerp(hShifted, YELLOW_HUE, exp2(-4.0 * pow2(hueDistL)) * shiftLFactor * SETTING_SKIBIDI_BB_SHIFT_L);
    hShifted = shortestPathLerp(hShifted, YELLOW_HUE, exp2(-3.0 * pow2(hueDistR)) * shiftLFactor * SETTING_SKIBIDI_BB_SHIFT_R);
    hShifted = shortestPathLerp(hShifted, BLUE_HUE, exp2(-3.0 * pow2(hueDistV)) * shiftLFactor * SETTING_SKIBIDI_BB_SHIFT_V);
    h = hShifted;

    float yellowFactor = pow2(smoothstep(PI, 0.0, hueDistY));

    float kb = mix(SETTING_SKIBIDI_BLUE_HIGHLIGHT_CHROMA_COMPRESS, SETTING_SKIBIDI_YELLOW_HIGHLIGHT_CHROMA_COMPRESS, yellowFactor);
    float kp = mix(SETTING_SKIBIDI_BLUE_HIGHLIGHT_CHROMA_COMPRESS_CONTRAST, SETTING_SKIBIDI_YELLOW_HIGHLIGHT_CHROMA_COMPRESS_CONTRAST, yellowFactor);

    float kbc = pow(kb, -kp);

    float pathFactor = kbc * rcp(pow(L, kp) + kbc);

    // Gamut boundary + chroma compression
    float C_max = _drt_findBoundaryChroma(L_compressed, h);

    float C_compressed = C;

    float S = C * safeRcp(C_max);
    S *= pow(pathFactor, inversesqrt(C * safeRcp(C_max)));
    C_compressed = S * C_max;
    C_compressed *= linearStep(0.0, 0.008, L); // Fixes blue toe

    float compressFactor = 5.5 - mix(SETTING_SKIBIDI_BLUE_GAMUT_COMPRESS, SETTING_SKIBIDI_YELLOW_GAMUT_COMPRESS, yellowFactor) * sqrt(smoothstep(0.0, 0.32, L)); // Lower number = more aggressive compression
    C_compressed = _drt_compressChroma(C_compressed, C_max, pow2    (compressFactor));

    // UCS polar -> CIE XYZ (relative) -> Output RGB -> OETF
    xyz = _drt_ucs_polarToXYZ(L_compressed, C_compressed, h);
    color = colors2_colorspaces_convert(COLORS2_COLORSPACES_CIE_XYZ, COLORS2_OUTPUT_COLORSPACE, xyz);

    color = saturate(color);
    color = colors2_oetf(COLORS2_OUTPUT_TF, color);

    const mat3 agx_mat = mat3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104
    );
    color.rgb = agx_mat * color.rgb;

    vec3 offset = vec3(SETTING_SKIBIDI_LOOK_OFFSET_R, SETTING_SKIBIDI_LOOK_OFFSET_G, SETTING_SKIBIDI_LOOK_OFFSET_B);
    vec3 slope = vec3(SETTING_SKIBIDI_LOOK_SLOPE_R, SETTING_SKIBIDI_LOOK_SLOPE_G, SETTING_SKIBIDI_LOOK_SLOPE_B);
    vec3 power = vec3(SETTING_SKIBIDI_LOOK_POWER_R, SETTING_SKIBIDI_LOOK_POWER_G, SETTING_SKIBIDI_LOOK_POWER_B);
    float sat = SETTING_SKIBIDI_LOOK_SATURATION;

    color = drt_look(color, offset, slope, power, SETTING_SKIBIDI_LOOK_SATURATION);

    const mat3 agx_mat_inv = mat3(
        1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116
    );
    color = agx_mat_inv * color;

    color = saturate(color);

    return color;
}

#endif
