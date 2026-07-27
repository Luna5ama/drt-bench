#pragma once

// OpenDRT v1.1.0 CPU port, Standard look.
// Original: Jed Smith (https://github.com/jedypod/open-display-transform)
// License: GPLv3

#include <numbers>
#include <glm/glm.hpp>

namespace odrt {
    constexpr float PI = std::numbers::pi_v<float>;
    constexpr float PI_2 = 2.0f * PI;

    // GLM is column-major — these match the GLSL mat3(col0, col1, col2) layout
    // XYZ-D65 -> P3-D65
    const glm::mat3 XYZ_TO_P3D65 = glm::mat3(
        2.49349691f,-0.82948897f,0.03584583f,
        -0.93138362f,1.76266406f,-0.07617239f,
        -0.40271078f,0.02362469f,0.95688452f
    );

    // P3-D65 -> XYZ-D65
    const glm::mat3 P3D65_TO_XYZ = glm::mat3(
        0.48657095f,0.22897456f,0.0f,
        0.26566769f,0.69173852f,0.04511338f,
        0.19821729f,0.07928691f,1.04394437f
    );

    // XYZ-D65 -> linear sRGB/Rec.709 (for colors2_colorspaces_convert)
    const glm::mat3 XYZ_TO_SRGB = glm::mat3(
        3.2404542f,-0.9692660f,0.0556434f,
        -1.5371385f,1.8760108f,-0.2040259f,
        -0.4985314f,0.0415560f,1.0572252f
    );

    inline float spow(float a, float b) {
        return a <= 0.0f ? a : std::pow(a, b);
    }

    inline float compress_hyp_pow(float x, float s, float p) {
        return spow(x / (x + s), p);
    }

    inline float toe_quad(float x, float toe, bool inv) {
        if (toe == 0.0f) return x;
        if (!inv) return x * x / (x + toe);
        return (x + std::sqrt(x * (4.0f * toe + x))) * 0.5f;
    }

    inline float softplus(float x, float s) {
        if (x > 10.0f * s || s < 1e-4f) return x;
        return s * std::log(glm::max(0.0f, 1.0f + std::exp(x / s)));
    }

    inline float gauss(float x, float w) {
        return std::exp(-x * x / w);
    }

    inline float hue_off(float h, float o) {
        return glm::mod(h - o + PI, PI_2) - PI;
    }

    inline glm::vec2 opponent(glm::vec3 rgb) {
        return glm::vec2(rgb.r - rgb.b, rgb.g - (rgb.r + rgb.b) * 0.5f);
    }

    // sRGB OETF
    inline float oetf_srgb(float x) {
        x = glm::clamp(x, 0.0f, 1.0f);
        return x < 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
    }

    inline glm::vec3 oetf_srgb(glm::vec3 c) {
        return glm::vec3(oetf_srgb(c.r), oetf_srgb(c.g), oetf_srgb(c.b));
    }

    // Saturate (component-wise clamp to [0,1])
    inline glm::vec3 saturate(glm::vec3 c) {
        return glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    }
} // namespace odrt

// Input:  scene-linear CIE XYZ-D65
// Output: display-encoded sRGB [0,1]
inline glm::vec3 openDRT(glm::vec3 color) {
    using namespace odrt;

    glm::vec3 rgb = glm::max(color, glm::vec3(0.0f));
    rgb = XYZ_TO_P3D65 * rgb;

    // --- Standard Look Preset Parameters ---
    const float tn_con = 1.66f, tn_sh = 0.5f, tn_toe = 0.003f, tn_off = 0.005f;
    const float tn_Lp = 100.0f, tn_gb = 0.13f, tn_Lg = 10.0f;
    const float pt_hdr = 0.5f;
    const int tn_su = 1;
    const float rs_sa = 0.35f, rs_rw = 0.25f, rs_bw = 0.55f;
    const float pt_lml = 0.25f, pt_lml_r = 0.5f, pt_lml_g = 0.0f, pt_lml_b = 0.1f;
    const float pt_lmh = 0.25f, pt_lmh_r = 0.5f, pt_lmh_b = 0.0f;
    const float ptl_c = 0.06f, ptl_m = 0.08f, ptl_y = 0.06f;
    const float ptm_low = 0.4f, ptm_low_rng = 0.25f, ptm_low_st = 0.5f;
    const float ptm_high = -0.8f, ptm_high_rng = 0.35f, ptm_high_st = 0.4f;
    const float brl = 0.0f, brl_r = -2.5f, brl_g = -1.5f, brl_b = -1.5f;
    const float brl_rng = 0.5f, brl_st = 0.35f;
    const float brlp = -0.5f, brlp_r = -1.25f, brlp_g = -1.25f, brlp_b = -0.25f;
    const float hc_r = 1.0f, hc_r_rng = 0.3f;
    const float hs_r = 0.6f, hs_r_rng = 0.6f, hs_g = 0.35f, hs_g_rng = 1.0f;
    const float hs_b = 0.66f, hs_b_rng = 1.0f;
    const float hs_c = 0.25f, hs_c_rng = 1.0f, hs_m = 0.0f, hs_m_rng = 1.0f;
    const float hs_y = 0.0f, hs_y_rng = 1.0f;

    // --- Tonescale constraints ---
    float ts_x1 = std::pow(2.0f, 6.0f * tn_sh + 4.0f);
    float ts_y1 = tn_Lp / 100.0f;
    float ts_x0 = 0.18f + tn_off;
    float ts_y0 = tn_Lg / 100.0f * (1.0f + tn_gb * std::log2(glm::max(ts_y1, 1e-10f)));
    float ts_s0 = toe_quad(ts_y0, tn_toe, true);
    float ts_p = tn_con / (1.0f + float(tn_su) * 0.05f);
    float ts_s10 = ts_x0 * (std::pow(ts_s0, -1.0f / tn_con) - 1.0f);
    float ts_m1 = ts_y1 / std::pow(ts_x1 / (ts_x1 + ts_s10), tn_con);
    float ts_m2 = toe_quad(ts_m1, tn_toe, true);
    float ts_s = ts_x0 * (std::pow(ts_s0 / ts_m2, -1.0f / tn_con) - 1.0f);
    float ts_dsc = 100.0f / tn_Lp;
    float pt_cmp_Lf = pt_hdr * glm::min(1.0f, (tn_Lp - 100.0f) / 900.0f);
    float s_Lp100 = ts_x0 * (std::pow(tn_Lg / 100.0f, -1.0f / tn_con) - 1.0f);
    float ts_s1 = ts_s * pt_cmp_Lf + s_Lp100 * (1.0f - pt_cmp_Lf);

    // --- Render Space ---
    glm::vec3 rs_w = glm::vec3(rs_rw, 1.0f - rs_rw - rs_bw, rs_bw);
    float sat_L = glm::dot(rgb, rs_w);
    rgb = glm::mix(rgb, glm::vec3(sat_L), rs_sa);
    rgb += tn_off;

    float tsn = glm::length(rgb) / 1.73205080757f;
    rgb = (tsn == 0.0f) ? glm::vec3(0.0f) : rgb / tsn;

    glm::vec2 opp = opponent(rgb);
    float ach_d = glm::length(opp) * 0.5f;
    ach_d = 1.25f * toe_quad(ach_d, 0.25f, false);

    float hue = glm::mod(std::atan2(opp.x, opp.y) + PI + 1.10714931f, PI_2);

    glm::vec3 ha_rgb = glm::vec3(
        gauss(hue_off(hue, 0.1f), 0.66f),
        gauss(hue_off(hue, 4.3f), 0.66f),
        gauss(hue_off(hue, 2.3f), 0.66f)
    );
    glm::vec3 ha_rgb_hs = glm::vec3(
        gauss(hue_off(hue, -0.4f), 0.66f),
        ha_rgb.y,
        gauss(hue_off(hue, 2.5f), 0.66f)
    );
    glm::vec3 ha_cmy = glm::vec3(
        gauss(hue_off(hue, 3.3f), 0.5f),
        gauss(hue_off(hue, 1.3f), 0.5f),
        gauss(hue_off(hue, -1.15f), 0.5f)
    );

    // Brilliance
    {
        float brl_tsf = std::pow(tsn / (tsn + 1.0f), 1.0f - brl_rng);
        float brl_exf = (brl + brl_r * ha_rgb.x + brl_g * ha_rgb.y + brl_b * ha_rgb.z)
                        * spow(ach_d, 1.0f / brl_st);
        float brl_ex = std::exp2(brl_exf * (brl_exf < 0.0f ? brl_tsf : 1.0f - brl_tsf));
        tsn *= brl_ex;
    }

    float tsn_pt = compress_hyp_pow(tsn, ts_s1, ts_p);
    float tsn_const = tsn_pt;
    tsn = compress_hyp_pow(tsn, ts_s, ts_p);

    // Hue Contrast R
    {
        float hc_ts = 1.0f - tsn_const;
        float hc_c = hc_ts * (1.0f - ach_d) + ach_d * (1.0f - hc_ts);
        hc_c *= ach_d * ha_rgb.x;
        hc_ts = spow(hc_ts, 1.0f / hc_r_rng);
        float hc_f = hc_r * (hc_c - 2.0f * hc_c * hc_ts) + 1.0f;
        rgb.g *= hc_f;
        rgb.b *= hc_f;
    }

    // Hue Shift RGB
    {
        glm::vec3 hs_rgb_v = glm::vec3(
            ha_rgb_hs.x * ach_d * spow(tsn_pt, 1.0f / hs_r_rng),
            ha_rgb_hs.y * ach_d * spow(tsn_pt, 1.0f / hs_g_rng),
            ha_rgb_hs.z * ach_d * spow(tsn_pt, 1.0f / hs_b_rng)
        );
        glm::vec3 hsf = glm::vec3(hs_rgb_v.x * hs_r, hs_rgb_v.y * -hs_g, hs_rgb_v.z * -hs_b);
        rgb += glm::vec3(hsf.z - hsf.y, hsf.x - hsf.z, hsf.y - hsf.x);
    }

    // Hue Shift CMY
    {
        float tsn_pt_c = 1.0f - tsn_pt;
        glm::vec3 hs_cmy_v = glm::vec3(
            ha_cmy.x * ach_d * spow(tsn_pt_c, 1.0f / hs_c_rng),
            ha_cmy.y * ach_d * spow(tsn_pt_c, 1.0f / hs_m_rng),
            ha_cmy.z * ach_d * spow(tsn_pt_c, 1.0f / hs_y_rng)
        );
        glm::vec3 hsf = glm::vec3(hs_cmy_v.x * -hs_c, hs_cmy_v.y * hs_m, hs_cmy_v.z * hs_y);
        rgb += glm::vec3(hsf.z - hsf.y, hsf.x - hsf.z, hsf.y - hsf.x);
    }

    // Purity Compression
    float pt_lml_p = 1.0f + 4.0f * (1.0f - tsn_pt)
                     * (pt_lml + pt_lml_r * ha_rgb_hs.x + pt_lml_g * ha_rgb_hs.y + pt_lml_b * ha_rgb_hs.z);
    float ptf = 1.0f - spow(tsn_pt, pt_lml_p);
    float pt_lmh_p = (1.0f - ach_d * (pt_lmh_r * ha_rgb_hs.x + pt_lmh_b * ha_rgb_hs.z))
                     * (1.0f - pt_lmh * ach_d);
    ptf = spow(ptf, pt_lmh_p);

    // Mid-Range Purity
    {
        float ptm_low_f = 1.0f + ptm_low * std::exp(-2.0f * ach_d * ach_d / ptm_low_st)
                          * spow(1.0f - tsn_const, 1.0f / ptm_low_rng);
        float ptm_high_f = 1.0f + ptm_high * std::exp(-2.0f * ach_d * ach_d / ptm_high_st)
                           * spow(tsn_pt, 1.0f / (4.0f * ptm_high_rng));
        ptf *= ptm_low_f * ptm_high_f;
    }

    rgb = rgb * ptf + (1.0f - ptf);

    // Inverse Render Space
    sat_L = glm::dot(rgb, rs_w);
    rgb = (sat_L * rs_sa - rgb) / (rs_sa - 1.0f);

    // P3D65 -> XYZ -> sRGB
    rgb = P3D65_TO_XYZ * rgb;
    rgb = XYZ_TO_SRGB * rgb;

    // Post Brilliance
    {
        glm::vec2 bp_opp = opponent(rgb);
        float bp_ach_d = glm::length(bp_opp) * 0.25f;
        bp_ach_d = 1.1f * (bp_ach_d * bp_ach_d / (bp_ach_d + 0.1f));
        glm::vec3 brlp_ha = ach_d * ha_rgb;
        float brlp_m = brlp + brlp_r * brlp_ha.x + brlp_g * brlp_ha.y + brlp_b * brlp_ha.z;
        rgb *= std::exp2(brlp_m * bp_ach_d * tsn);
    }

    // Purity Compress Low
    rgb = glm::vec3(softplus(rgb.r, ptl_c), softplus(rgb.g, ptl_m), softplus(rgb.b, ptl_y));

    // Final Tonescale
    tsn *= ts_m2;
    tsn = toe_quad(tsn, tn_toe, false);
    tsn *= ts_dsc;
    rgb *= tsn;

    // Clamp, apply sRGB OETF, identity look (no AGX matrices needed)
    color = saturate(rgb);
    color = oetf_srgb(color);
    color = saturate(color);
    return color;
}