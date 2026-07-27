#ifndef INCLUDE_techniques_drt_AgX_glsl
#define INCLUDE_techniques_drt_AgX_glsl a

#include "Common.glsl"

// All values used to derive this implementation are sourced from Troy’s initial AgX implementation/OCIO config file available here:
//   https://github.com/sobotka/AgX
vec3 agxDefaultContrastApprox(vec3 x) {
    const float scale0 = 59.507875;
    const float scale1 = 69.862789;

    x -= 20.0 / 33.0;
    vec3 type = vec3(floatBitsToUint(x) >> 31);

    vec3 scale = scale1 + (scale0 - scale1) * type;
    vec3 power0 = 13.0 / 4.0 - 0.25 * type;
    vec3 power1 = -4.0 / 13.0 + (4.0 / 13.0 - 1.0 / 3.0) * type;

    x = 2.0 * x * pow(1.0 + scale * pow(abs(x), power0), power1) + 0.5;
    return x;
}

vec3 agx(vec3 val) {
    const mat3 agx_mat = mat3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104
    );

    // Input transform
    val = agx_mat * val;

    // Log2 space encoding
    //    const float EV_RANGE = SETTING_TONE_MAPPING_DYNAMIC_RANGE;
    //    const float EV_RANGE_HALF = EV_RANGE * 0.5;
    //    val = clamp(log2(val), -EV_RANGE_HALF, EV_RANGE_HALF);
    //    val = (val + EV_RANGE_HALF) / EV_RANGE;
    const float min_ev = -12.473931188332413;
    const float max_ev = 4.026068811667588;
    val = linearStep(min_ev, max_ev, log2(val));

    // Apply sigmoid function approximation
    val = agxDefaultContrastApprox(val);

    return val;
}

vec3 agxEotf(vec3 val) {
    const mat3 agx_mat_inv = mat3(
        1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116
    );

    // Inverse input transform (outset)
    val = agx_mat_inv * val;

    // sRGB IEC 61966-2-1 2.2 Exponent Reference EOTF Display
    // NOTE: We're linearizing the output here. Comment/adjust when
    // *not* using a sRGB render target
    val = colors2_eotf(COLORS2_TF_SRGB, val);

    return val;
}

vec3 agxLook(vec3 val) {
    // Default
    vec3 offset = vec3(0.0);
    vec3 slope = vec3(1.0);
    vec3 power = vec3(1.0);
    float sat = 1.0;

    #if SETTING_AGX_LOOK == 1
    // Golden
    slope = vec3(1.0, 0.9, 0.5);
    power = vec3(0.8);
    sat = 0.8;
    #elif SETTING_AGX_LOOK == 2
    // Punchy
    slope = vec3(1.0);
    power = vec3(1.35, 1.35, 1.35);
    sat = 1.4;
    #elif SETTING_AGX_LOOK == 3
    // Custom
    offset = vec3(SETTING_AGX_LOOK_OFFSET_R, SETTING_AGX_LOOK_OFFSET_G, SETTING_AGX_LOOK_OFFSET_B);
    slope = vec3(SETTING_AGX_LOOK_SLOPE_R, SETTING_AGX_LOOK_SLOPE_G, SETTING_AGX_LOOK_SLOPE_B);
    power = vec3(SETTING_AGX_LOOK_POWER_R, SETTING_AGX_LOOK_POWER_G, SETTING_AGX_LOOK_POWER_B);
    sat = SETTING_AGX_LOOK_SATURATION;
    #endif

    return drt_look(val, offset, slope, power, sat);
}

vec3 drt_agx(vec3 color) {
    color = colors2_colorspaces_convert(COLORS2_COLORSPACES_CIE_XYZ, COLORS2_DRT_WORKING_COLORSPACE, color);
    color = max(color, 0.0);

    color = agx(color);
    color = agxLook(color);
    color = agxEotf(color);

    color = colors2_colorspaces_convert(COLORS2_DRT_WORKING_COLORSPACE, COLORS2_OUTPUT_COLORSPACE, color);
    color = saturate(color);

    color = colors2_oetf(COLORS2_OUTPUT_TF, color);
    color = saturate(color);

    return color;
}

#endif