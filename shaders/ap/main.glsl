// 0 = AgX, 1 = OpenDRT, 2 = Skibidi
#define DRT_BENCH_DRT 1

// Alpha-Piscium defaults. These are compatibility settings, not DRT core edits.
#define SETTING_DRT_WORKING_COLOR_SPACE 1

#define SETTING_AGX_LOOK 3
#define SETTING_AGX_LOOK_OFFSET_R 0.0
#define SETTING_AGX_LOOK_OFFSET_G 0.0
#define SETTING_AGX_LOOK_OFFSET_B 0.0
#define SETTING_AGX_LOOK_SLOPE_R 1.05
#define SETTING_AGX_LOOK_SLOPE_G 1.05
#define SETTING_AGX_LOOK_SLOPE_B 1.05
#define SETTING_AGX_LOOK_POWER_R 1.1
#define SETTING_AGX_LOOK_POWER_G 1.1
#define SETTING_AGX_LOOK_POWER_B 1.1
#define SETTING_AGX_LOOK_SATURATION 1.3

#define SETTING_OPENDRT_LOOK_OFFSET_R 0.0
#define SETTING_OPENDRT_LOOK_OFFSET_G 0.0
#define SETTING_OPENDRT_LOOK_OFFSET_B 0.0
#define SETTING_OPENDRT_LOOK_SLOPE_R 1.0
#define SETTING_OPENDRT_LOOK_SLOPE_G 1.0
#define SETTING_OPENDRT_LOOK_SLOPE_B 1.0
#define SETTING_OPENDRT_LOOK_POWER_R 1.0
#define SETTING_OPENDRT_LOOK_POWER_G 1.0
#define SETTING_OPENDRT_LOOK_POWER_B 1.0
#define SETTING_OPENDRT_LOOK_SATURATION 1.2

#define SETTING_SKIBIDI_LOOK_OFFSET_R 0.0
#define SETTING_SKIBIDI_LOOK_OFFSET_G 0.0
#define SETTING_SKIBIDI_LOOK_OFFSET_B 0.0
#define SETTING_SKIBIDI_LOOK_SLOPE_R 1.0
#define SETTING_SKIBIDI_LOOK_SLOPE_G 1.0
#define SETTING_SKIBIDI_LOOK_SLOPE_B 1.0
#define SETTING_SKIBIDI_LOOK_POWER_R 1.1
#define SETTING_SKIBIDI_LOOK_POWER_G 1.1
#define SETTING_SKIBIDI_LOOK_POWER_B 1.1
#define SETTING_SKIBIDI_LOOK_SATURATION 1.35
#define SETTING_SKIBIDI_BB_SHIFT_C 1.0
#define SETTING_SKIBIDI_BB_SHIFT_L 1.0
#define SETTING_SKIBIDI_BB_SHIFT_R 1.0
#define SETTING_SKIBIDI_BB_SHIFT_V 1.0
#define SETTING_SKIBIDI_LINEAR_SLOPE 4.8
#define SETTING_SKIBIDI_LINEAR_LENGTH 0.13
#define SETTING_SKIBIDI_LINEAR_STARTX 0.39
#define SETTING_SKIBIDI_LINEAR_STARTY 0.18
#define SETTING_SKIBIDI_TOE_SLOPE 0.002
#define SETTING_SKIBIDI_SHOULDER_SLOPE 0.02
#define SETTING_SKIBIDI_BLUE_HIGHLIGHT_CHROMA_COMPRESS 5.2
#define SETTING_SKIBIDI_BLUE_HIGHLIGHT_CHROMA_COMPRESS_CONTRAST 4.0
#define SETTING_SKIBIDI_YELLOW_HIGHLIGHT_CHROMA_COMPRESS 4.8
#define SETTING_SKIBIDI_YELLOW_HIGHLIGHT_CHROMA_COMPRESS_CONTRAST 2.5
#define SETTING_SKIBIDI_BLUE_GAMUT_COMPRESS 4.3
#define SETTING_SKIBIDI_YELLOW_GAMUT_COMPRESS 3.5

#if DRT_BENCH_DRT == 0
#include "techniques/drt/AgX.glsl"
#elif DRT_BENCH_DRT == 1
#include "techniques/drt/OpenDRT.glsl"
#elif DRT_BENCH_DRT == 2
#include "techniques/drt/Skibidi.glsl"
#else
#error DRT_BENCH_DRT must be 0 (AgX), 1 (OpenDRT), or 2 (Skibidi)
#endif

vec3 applyDrt(vec3 xyz) {
#if DRT_BENCH_DRT == 0
    return drt_agx(xyz);
#elif DRT_BENCH_DRT == 1
    return drt_openDRT(xyz);
#else
    return drt_skibidi(xyz);
#endif
}

vec3 encodeBenchOutput(vec3 srgb) {
#ifdef DRT_BENCH_HDR
    vec3 linearSrgb = colors2_eotf(COLORS2_TF_SRGB, srgb);
    vec3 rec2020 = colors2_colorspaces_convert(COLORS2_COLORSPACES_SRGB, COLORS2_COLORSPACES_REC2020, linearSrgb);
    return colors2_oetf_PQ(max(rec2020, 0.0) * 203.0);
#else
    return srgb;
#endif
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uimg_outputTex);
    if (any(greaterThanEqual(pixel, size))) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    vec3 ap0 = texture(usam_inputTex, uv).rgb;
    vec3 xyz = colors2_colorspaces_convert(COLORS2_COLORSPACES_ACES_AP0, COLORS2_COLORSPACES_CIE_XYZ, ap0);
    imageStore(uimg_outputTex, pixel, vec4(encodeBenchOutput(applyDrt(xyz)), 1.0));
}
