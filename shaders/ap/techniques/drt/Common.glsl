#ifndef INCLUDE_techniques_drt_Common_glsl
#define INCLUDE_techniques_drt_Common_glsl a

#include "/util/Colors2.glsl"

#define USE_LOOK 0

vec3 drt_look(vec3 color, vec3 offset, vec3 slope, vec3 power, float saturation) {
    #if USE_LOOK
    color = pow(color * slope + offset, power);
    float luma = colors2_colorspaces_luma(COLORS2_DRT_WORKING_COLORSPACE, color);
    color = luma + saturation * (color - luma);
    #endif
    return color;
}

#endif
