#pragma once

#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

enum class CubeSpace : std::size_t {
    rgb,
    xyY,
    cieYuv,
    cieLuv,
    cieLab,
    cieLch,
    jzAzBz,
    jzCzHz,
    count,
};

struct CubeDescription {
    const wchar_t* title;
    const wchar_t* first;
    const wchar_t* second;
    const wchar_t* vertical;
};

inline const CubeDescription& cubeDescription(CubeSpace space) {
    static constexpr CubeDescription descriptions[] = {
        {L"Output RGB Cube", L"R", L"G", L"B"},
        {L"Output xyY Cube", L"y", L"x", L"Y"},
        {L"Output CIE YUV Cube", L"v", L"u", L"Y"},
        {L"Output CIELUV Cube", L"v*", L"u*", L"L*"},
        {L"Output CIELAB Cube", L"b*", L"a*", L"L*"},
        {L"Output CIELCh Cube", L"h", L"C*", L"L*"},
        {L"Output JzAzBz Cube", L"Bz", L"Az", L"Jz"},
        {L"Output JzCzHz Cube", L"hz", L"Cz", L"Jz"},
    };
    return descriptions[static_cast<std::size_t>(space)];
}

struct CubePoint {
    float x;
    float y;
};

struct Xyz {
    float x;
    float y;
    float z;
};

struct XyY {
    float x;
    float y;
    float bigY;
};

struct ColorTriplet {
    float first;
    float second;
    float vertical;
};

inline CubePoint projectCube(float first, float second, float vertical,
                             float sinYaw = 0.0f, float cosYaw = 1.0f,
                             float sinPitch = 0.0f, float cosPitch = 1.0f) {
    constexpr float halfHorizontal = 0.4330127f;
    constexpr float inverseSqrtTwo = 0.7071068f;
    const float centeredFirst = first - 0.5f;
    const float centeredSecond = second - 0.5f;
    const float centeredVertical = vertical - 0.5f;
    const float yawFirst = cosYaw * centeredFirst - sinYaw * centeredSecond;
    const float yawSecond = sinYaw * centeredFirst + cosYaw * centeredSecond;
    const float pitchDot = (yawFirst + yawSecond) * inverseSqrtTwo;
    const float pitchFirst = yawFirst * cosPitch + centeredVertical * inverseSqrtTwo * sinPitch +
        pitchDot * inverseSqrtTwo * (1.0f - cosPitch);
    const float pitchSecond = yawSecond * cosPitch - centeredVertical * inverseSqrtTwo * sinPitch +
        pitchDot * inverseSqrtTwo * (1.0f - cosPitch);
    const float pitchVertical = centeredVertical * cosPitch +
        (yawSecond - yawFirst) * inverseSqrtTwo * sinPitch;
    return {0.5f + (pitchFirst + pitchSecond) * halfHorizontal,
            0.5f + 0.25f * (pitchFirst - pitchSecond) + 0.5f * pitchVertical};
}

inline float decodeDisplayValue(float value, bool hdr10) {
    if (!hdr10) {
        return value <= 0.04045f ? value / 12.92f :
            std::pow((value + 0.055f) / 1.055f, 2.4f);
    }
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    const float power = std::pow(value, 1.0f / m2);
    return std::pow(std::max(power - c1, 0.0f) / (c2 - c3 * power), 1.0f / m1);
}

inline Xyz linearRgbToXyz(float red, float green, float blue, bool hdr10) {
    return hdr10 ? Xyz{
        0.6369580483f * red + 0.1446169036f * green + 0.1688809752f * blue,
        0.2627002120f * red + 0.6779980715f * green + 0.0593017165f * blue,
        0.0280726930f * green + 1.0609850577f * blue,
    } : Xyz{
        0.4123907993f * red + 0.3575843394f * green + 0.1804807884f * blue,
        0.2126390059f * red + 0.7151686788f * green + 0.0721923154f * blue,
        0.0193308187f * red + 0.1191947798f * green + 0.9505321522f * blue,
    };
}

inline Xyz rgbToXyz(float red, float green, float blue, bool hdr10) {
    return linearRgbToXyz(decodeDisplayValue(red, hdr10),
                          decodeDisplayValue(green, hdr10),
                          decodeDisplayValue(blue, hdr10), hdr10);
}
inline XyY xyzToXyY(const Xyz& xyz) {
    const float sum = xyz.x + xyz.y + xyz.z;
    return sum > 0.0f ? XyY{xyz.x / sum, xyz.y / sum, xyz.y} : XyY{};
}

inline ColorTriplet xyzToCieYuv(const Xyz& xyz) {
    const float denominator = xyz.x + 15.0f * xyz.y + 3.0f * xyz.z;
    return denominator > 0.0f ? ColorTriplet{
        6.0f * xyz.y / denominator, 4.0f * xyz.x / denominator, xyz.y,
    } : ColorTriplet{};
}

inline float cieLightnessFunction(float value) {
    constexpr float epsilon = 216.0f / 24389.0f;
    return value > epsilon ? std::cbrt(value) : (841.0f / 108.0f) * value + 4.0f / 29.0f;
}

inline ColorTriplet xyzToCieLuv(const Xyz& xyz) {
    constexpr float whiteX = 0.9504559f;
    constexpr float whiteZ = 1.0890578f;
    constexpr float whiteDenominator = whiteX + 15.0f + 3.0f * whiteZ;
    constexpr float whiteU = 4.0f * whiteX / whiteDenominator;
    constexpr float whiteV = 9.0f / whiteDenominator;
    const float denominator = xyz.x + 15.0f * xyz.y + 3.0f * xyz.z;
    const float u = denominator > 0.0f ? 4.0f * xyz.x / denominator : whiteU;
    const float v = denominator > 0.0f ? 9.0f * xyz.y / denominator : whiteV;
    const float lightness = 116.0f * cieLightnessFunction(xyz.y) - 16.0f;
    return {13.0f * lightness * (v - whiteV),
            13.0f * lightness * (u - whiteU), lightness};
}

inline ColorTriplet xyzToCieLab(const Xyz& xyz) {
    const float fx = cieLightnessFunction(xyz.x / 0.9504559f);
    const float fy = cieLightnessFunction(xyz.y);
    const float fz = cieLightnessFunction(xyz.z / 1.0890578f);
    return {200.0f * (fy - fz), 500.0f * (fx - fy), 116.0f * fy - 16.0f};
}

inline ColorTriplet xyzToJzAzBz(const Xyz& relativeXyz, bool hdr10) {
    const float scale = hdr10 ? 10000.0f : 203.0f;
    const float x = relativeXyz.x * scale;
    const float y = relativeXyz.y * scale;
    const float z = relativeXyz.z * scale;
    const float xp = 1.15f * x - 0.15f * z;
    const float yp = 0.66f * y + 0.34f * x;
    const float lp = 0.41478972f * xp + 0.579999f * yp + 0.014648f * z;
    const float mp = -0.20151f * xp + 1.120649f * yp + 0.0531008f * z;
    const float sp = -0.0166008f * xp + 0.2648f * yp + 0.6684799f * z;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    constexpr float n = 2610.0f / 16384.0f;
    constexpr float p = 1.7f * 2523.0f / 32.0f;
    const auto pq = [](float value) {
        const float power = std::pow(std::max(value, 0.0f) / 10000.0f, n);
        return std::pow((c1 + c2 * power) / (1.0f + c3 * power), p);
    };
    const float l = pq(lp);
    const float m = pq(mp);
    const float s = pq(sp);
    const float iz = 0.5f * (l + m);
    const float az = 3.524f * l - 4.066708f * m + 0.542708f * s;
    const float bz = 0.199076f * l + 1.096799f * m - 1.295875f * s;
    constexpr float d = -0.56f;
    constexpr float d0 = 1.6295499532821566e-11f;
    const float jz = (1.0f + d) * iz / (1.0f + d * iz) - d0;
    return {bz, az, jz};
}

struct AxisRange {
    float minimum;
    float maximum;
};

struct CubeRanges {
    AxisRange first;
    AxisRange second;
    AxisRange vertical;
};

struct Chromaticity {
    float x;
    float y;
};

// 5 nm subset of the CIE 1931 2-degree spectral locus, CIE 018:2019 Table 6.
inline constexpr Chromaticity cie1931SpectralLocus[] = {
    {0.17556f, 0.00529f},
    {0.17516f, 0.00526f},
    {0.17482f, 0.00522f},
    {0.17451f, 0.00518f},
    {0.17411f, 0.00496f},
    {0.17401f, 0.00498f},
    {0.1738f, 0.00492f},
    {0.17356f, 0.00492f},
    {0.17334f, 0.0048f},
    {0.17302f, 0.00478f},
    {0.17258f, 0.0048f},
    {0.17209f, 0.00483f},
    {0.17141f, 0.0051f},
    {0.1703f, 0.00579f},
    {0.16888f, 0.0069f},
    {0.1669f, 0.00855f},
    {0.16441f, 0.01086f},
    {0.16111f, 0.01379f},
    {0.15664f, 0.01771f},
    {0.15099f, 0.02274f},
    {0.14396f, 0.0297f},
    {0.1355f, 0.03988f},
    {0.12412f, 0.0578f},
    {0.1096f, 0.08684f},
    {0.09129f, 0.1327f},
    {0.06871f, 0.20072f},
    {0.04539f, 0.29498f},
    {0.02346f, 0.4127f},
    {0.00817f, 0.53842f},
    {0.00386f, 0.65482f},
    {0.01387f, 0.75019f},
    {0.03885f, 0.81202f},
    {0.0743f, 0.8338f},
    {0.11416f, 0.82621f},
    {0.15472f, 0.80586f},
    {0.19288f, 0.78163f},
    {0.22962f, 0.75433f},
    {0.26578f, 0.72432f},
    {0.3016f, 0.69231f},
    {0.33736f, 0.65885f},
    {0.3731f, 0.62445f},
    {0.40873f, 0.58961f},
    {0.44406f, 0.55472f},
    {0.47878f, 0.5202f},
    {0.51249f, 0.48659f},
    {0.54479f, 0.45443f},
    {0.57515f, 0.42423f},
    {0.60293f, 0.3965f},
    {0.62704f, 0.37249f},
    {0.64823f, 0.3514f},
    {0.66576f, 0.33401f},
    {0.68008f, 0.31975f},
    {0.69151f, 0.30834f},
    {0.70061f, 0.2993f},
    {0.70792f, 0.29203f},
    {0.71403f, 0.28593f},
    {0.71903f, 0.28094f},
    {0.72303f, 0.27695f},
    {0.72599f, 0.27401f},
    {0.72827f, 0.27173f},
    {0.72997f, 0.27003f},
    {0.73109f, 0.26891f},
    {0.73199f, 0.26801f},
    {0.73272f, 0.26728f},
    {0.73342f, 0.26658f},
    {0.73405f, 0.26595f},
    {0.73439f, 0.26561f},
    {0.73459f, 0.26541f},
    {0.73469f, 0.26531f},
};

inline float remapColorCoordinate(float value, AxisRange range) {
    const float span = range.maximum - range.minimum;
    return span > 0.0f ? std::clamp((value - range.minimum) / span, 0.0f, 1.0f) : 0.0f;
}

inline ColorTriplet rawCubeCoordinates(CubeSpace space, const Xyz& xyz, bool hdr10) {
    switch (space) {
    case CubeSpace::xyY: {
        const XyY value = xyzToXyY(xyz);
        return {value.y, value.x, value.bigY};
    }
    case CubeSpace::cieYuv:
        return xyzToCieYuv(xyz);
    case CubeSpace::cieLuv:
        return xyzToCieLuv(xyz);
    case CubeSpace::cieLab:
        return xyzToCieLab(xyz);
    case CubeSpace::cieLch: {
        const ColorTriplet lab = xyzToCieLab(xyz);
        const float chroma = std::hypot(lab.second, lab.first);
        float hue = chroma > 1.0e-4f ? std::atan2(lab.first, lab.second) * 57.2957795f : 0.0f;
        if (hue < 0.0f) hue += 360.0f;
        return {hue, chroma, lab.vertical};
    }
    case CubeSpace::jzAzBz:
        return xyzToJzAzBz(xyz, hdr10);
    case CubeSpace::jzCzHz: {
        const ColorTriplet value = xyzToJzAzBz(xyz, hdr10);
        const float rawChroma = std::hypot(value.second, value.first);
        const float chroma = rawChroma > 1.0e-7f ? rawChroma : 0.0f;
        float hue = chroma > 0.0f ? std::atan2(value.first, value.second) * 57.2957795f : 0.0f;
        if (hue < 0.0f) hue += 360.0f;
        return {hue, chroma, value.vertical};
    }
    case CubeSpace::rgb:
    case CubeSpace::count:
        return {};
    }
    return {};
}

inline CubeRanges cubeRanges(CubeSpace space, bool hdr10 = false, float peakRelative = 1.0f) {
    switch (space) {
    case CubeSpace::rgb:
        return {{0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 1.0f}};
    case CubeSpace::xyY:
        return {{0.00477f, 0.83409f}, {0.00364f, 0.73469f}, {0.0f, 1.0f}};
    case CubeSpace::cieYuv:
        return {{0.0105568f, 0.3911726f}, {0.0013750f, 0.6233662f}, {0.0f, 1.0f}};
    case CubeSpace::cieLuv:
        return {{-142.0f, 133.0f}, {-159.0f, 272.0f}, {0.0f, 100.0f}};
    case CubeSpace::cieLab:
        return {{-121.0f, 137.0f}, {-173.0f, 131.0f}, {0.0f, 100.0f}};
    case CubeSpace::cieLch: {
        float maximumChroma = 0.0f;
        for (int vertex = 1; vertex < 8; ++vertex) {
            const Xyz xyz = linearRgbToXyz(static_cast<float>((vertex >> 0) & 1),
                                            static_cast<float>((vertex >> 1) & 1),
                                            static_cast<float>((vertex >> 2) & 1), hdr10);
            maximumChroma = std::max(maximumChroma, rawCubeCoordinates(space, xyz, hdr10).second);
        }
        return {{0.0f, 360.0f}, {0.0f, maximumChroma}, {0.0f, 100.0f}};
    }
    case CubeSpace::jzAzBz:
    case CubeSpace::jzCzHz: {
        peakRelative = std::clamp(peakRelative, 0.0f, 1.0f);
        if (peakRelative == 0.0f) {
            return space == CubeSpace::jzCzHz
                ? CubeRanges{{0.0f, 360.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}}
                : CubeRanges{};
        }
        float minimumBz = 0.0f;
        float maximumBz = 0.0f;
        float minimumAz = 0.0f;
        float maximumAz = 0.0f;
        float maximumCz = 0.0f;
        const auto includeChromaticity = [&](Chromaticity chromaticity) {
            const Xyz xyz{
                chromaticity.x / chromaticity.y * peakRelative,
                peakRelative,
                (1.0f - chromaticity.x - chromaticity.y) / chromaticity.y * peakRelative,
            };
            const ColorTriplet value = xyzToJzAzBz(xyz, hdr10);
            minimumBz = std::min(minimumBz, value.first);
            maximumBz = std::max(maximumBz, value.first);
            minimumAz = std::min(minimumAz, value.second);
            maximumAz = std::max(maximumAz, value.second);
            maximumCz = std::max(maximumCz, std::hypot(value.first, value.second));
        };
        for (const Chromaticity chromaticity : cie1931SpectralLocus) {
            includeChromaticity(chromaticity);
        }
        constexpr int purpleSteps = 64;
        const Chromaticity violet = cie1931SpectralLocus[0];
        const Chromaticity red = cie1931SpectralLocus[
            std::size(cie1931SpectralLocus) - 1];
        for (int step = 1; step < purpleSteps; ++step) {
            const float t = static_cast<float>(step) / purpleSteps;
            includeChromaticity({
                violet.x + (red.x - violet.x) * t,
                violet.y + (red.y - violet.y) * t,
            });
        }
        const float maximumJz = xyzToJzAzBz(
            linearRgbToXyz(peakRelative, peakRelative, peakRelative, hdr10), hdr10).vertical;
        if (space == CubeSpace::jzCzHz) {
            return {{0.0f, 360.0f}, {0.0f, maximumCz}, {0.0f, maximumJz}};
        }
        return {{minimumBz, maximumBz}, {minimumAz, maximumAz}, {0.0f, maximumJz}};
    }
    case CubeSpace::count:
        return {};
    }
    return {};
}

inline ColorTriplet normalizeCubeCoordinates(const ColorTriplet& value, const CubeRanges& ranges) {
    return {remapColorCoordinate(value.first, ranges.first),
            remapColorCoordinate(value.second, ranges.second),
            remapColorCoordinate(value.vertical, ranges.vertical)};
}

inline ColorTriplet cubeCoordinates(CubeSpace space, const Xyz& xyz, bool hdr10) {
    return normalizeCubeCoordinates(rawCubeCoordinates(space, xyz, hdr10),
                                    cubeRanges(space, hdr10));
}
class ColorCubeWindow {
public:
    ~ColorCubeWindow() {
        if (window_) DestroyWindow(window_);
    }

    void create(HWND owner, CubeSpace space) {
        space_ = space;
        ranges_ = cubeRanges(space);
        const HINSTANCE module = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = module;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"drt-bench-color-cube";
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

        constexpr int width = 500;
        constexpr int height = 500;
        RECT ownerBounds{};
        RECT work{};
        GetWindowRect(owner, &ownerBounds);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const std::size_t index = static_cast<std::size_t>(space);
        const bool onRight = index % 2 == 0;
        const int cascade = static_cast<int>(index / 2) * 28;
        int x = onRight ? ownerBounds.right + 12 + cascade : ownerBounds.left - width - 12 - cascade;
        if (onRight && x + width > work.right) x = ownerBounds.left - width - 12 - cascade;
        if (!onRight && x < work.left) x = ownerBounds.right + 12 + cascade;
        x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
        const int y = std::clamp(static_cast<int>(ownerBounds.top) + cascade,
                                 static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName,
                                  cubeDescription(space).title,
                                  WS_CAPTION | WS_THICKFRAME,
                                  x, y, width, height, owner, nullptr, module, this);
        if (window_) ShowWindow(window_, SW_SHOWNOACTIVATE);
    }

    bool visible() const {
        return window_ && IsWindowVisible(window_);
    }

    static bool supports(VkFormat format) {
        return format == VK_FORMAT_R8G8B8A8_UNORM ||
               format == VK_FORMAT_B8G8R8A8_UNORM ||
               format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    }

    void clear() {
        samples_.clear();
        if (window_) InvalidateRect(window_, nullptr, FALSE);
    }

    static float peakRelative(const void* pixels, uint32_t width, uint32_t height, VkFormat format) {
        if (!pixels || !width || !height || !supports(format)) return 1.0f;
        const size_t pixelCount = static_cast<size_t>(width) * height;
        float maximumY = 0.0f;
        if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            std::array<float, 1024> decoded{};
            for (size_t value = 0; value < decoded.size(); ++value) {
                decoded[value] = decodeDisplayValue(
                    static_cast<float>(value) / (decoded.size() - 1), true);
            }
            const auto* packed = static_cast<const uint32_t*>(pixels);
            for (size_t index = 0; index < pixelCount; ++index) {
                const uint32_t value = packed[index];
                const float red = decoded[(value >> 0) & 1023u];
                const float green = decoded[(value >> 10) & 1023u];
                const float blue = decoded[(value >> 20) & 1023u];
                maximumY = std::max(maximumY,
                    0.2627002120f * red + 0.6779980715f * green + 0.0593017165f * blue);
            }
            return maximumY;
        }
        std::array<float, 256> decoded{};
        for (size_t value = 0; value < decoded.size(); ++value) {
            decoded[value] = decodeDisplayValue(
                static_cast<float>(value) / (decoded.size() - 1), false);
        }
        const auto* bytes = static_cast<const uint8_t*>(pixels);
        const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
        for (size_t index = 0; index < pixelCount; ++index) {
            const size_t offset = index * 4;
            const float red = decoded[bytes[offset + (bgra ? 2 : 0)]];
            const float green = decoded[bytes[offset + 1]];
            const float blue = decoded[bytes[offset + (bgra ? 0 : 2)]];
            maximumY = std::max(maximumY,
                0.2126390059f * red + 0.7151686788f * green + 0.0721923154f * blue);
        }
        return maximumY;
    }
    void update(const void* pixels, uint32_t width, uint32_t height, VkFormat format,
                float peakRelative) {
        if (!window_ || !pixels || !width || !height || !supports(format)) return;
        constexpr size_t maxSamples = 100'000;
        const size_t pixelCount = static_cast<size_t>(width) * height;
        const size_t step = std::max<size_t>(1, static_cast<size_t>(
            std::ceil(std::sqrt(static_cast<double>(pixelCount) / maxSamples))));
        samples_.clear();
        samples_.reserve(std::min(maxSamples, (pixelCount + step * step - 1) / (step * step)));
        const auto* bytes = static_cast<const uint8_t*>(pixels);
        const auto* packed = static_cast<const uint32_t*>(pixels);
        const bool hdr10 = format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        ranges_ = cubeRanges(space_, hdr10, peakRelative);
        for (uint32_t y = 0; y < height; y += static_cast<uint32_t>(step)) {
            for (uint32_t x = 0; x < width; x += static_cast<uint32_t>(step)) {
                const size_t index = static_cast<size_t>(y) * width + x;
                Sample sample{};
                float red = 0.0f;
                float green = 0.0f;
                float blue = 0.0f;
                if (hdr10) {
                    const uint32_t value = packed[index];
                    red = static_cast<float>((value >> 0) & 1023) / 1023.0f;
                    green = static_cast<float>((value >> 10) & 1023) / 1023.0f;
                    blue = static_cast<float>((value >> 20) & 1023) / 1023.0f;
                    sample.red = static_cast<uint8_t>(std::lround(red * 255.0f));
                    sample.green = static_cast<uint8_t>(std::lround(green * 255.0f));
                    sample.blue = static_cast<uint8_t>(std::lround(blue * 255.0f));
                } else {
                    const size_t offset = index * 4;
                    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
                    sample.red = bytes[offset + (bgra ? 2 : 0)];
                    sample.green = bytes[offset + 1];
                    sample.blue = bytes[offset + (bgra ? 0 : 2)];
                    red = sample.red / 255.0f;
                    green = sample.green / 255.0f;
                    blue = sample.blue / 255.0f;
                }
                if (space_ == CubeSpace::rgb) {
                    sample.first = red;
                    sample.second = green;
                    sample.vertical = blue;
                } else {
                    const Xyz xyz = linearRgbToXyz(decodeDisplayValue(red, hdr10),
                                                    decodeDisplayValue(green, hdr10),
                                                    decodeDisplayValue(blue, hdr10), hdr10);
                    const ColorTriplet coordinates =
                        normalizeCubeCoordinates(rawCubeCoordinates(space_, xyz, hdr10), ranges_);
                    sample.first = coordinates.first;
                    sample.second = coordinates.second;
                    sample.vertical = coordinates.vertical;
                }
                samples_.push_back(sample);
            }
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

private:
    struct Sample {
        uint8_t red;
        uint8_t green;
        uint8_t blue;
        float first;
        float second;
        float vertical;
    };

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ColorCubeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<ColorCubeWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: self->paint(window); return 0;
        case WM_SIZE: InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN:
            self->dragging_ = true;
            self->dragPoint_ = MAKEPOINTS(lparam);
            SetCapture(window);
            return 0;
        case WM_MOUSEMOVE:
            if (self->dragging_ && (wparam & MK_LBUTTON)) {
                const POINTS point = MAKEPOINTS(lparam);
                self->yaw_ += (point.x - self->dragPoint_.x) * 0.01f;
                self->pitch_ = std::clamp(self->pitch_ + (point.y - self->dragPoint_.y) * 0.01f,
                                          -1.4f, 1.4f);
                self->dragPoint_ = point;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (self->dragging_) {
                self->dragging_ = false;
                if (GetCapture() == window) ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED: self->dragging_ = false; return 0;
        case WM_RBUTTONUP:
            self->yaw_ = 0.0f;
            self->pitch_ = 0.0f;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_CLOSE: return 0;
        case WM_NCDESTROY:
            self->window_ = nullptr;
            return DefWindowProcW(window, message, wparam, lparam);
        default: return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    static uint32_t color(const Sample& sample) {
        return static_cast<uint32_t>(sample.red) << 16 |
               static_cast<uint32_t>(sample.green) << 8 | sample.blue;
    }

    static POINT screenPoint(float first, float second, float vertical,
                             float sinYaw, float cosYaw, float sinPitch, float cosPitch,
                             int size, int left, int bottom) {
        const CubePoint point = projectCube(first, second, vertical,
                                            sinYaw, cosYaw, sinPitch, cosPitch);
        return {left + static_cast<LONG>(std::lround(point.x * size)),
                bottom - static_cast<LONG>(std::lround(point.y * size))};
    }

    static void putPixel(std::vector<uint32_t>& bitmap, int width, int height,
                         int x, int y, uint32_t value) {
        if (x >= 0 && y >= 0 && x < width && y < height) {
            bitmap[static_cast<size_t>(y) * width + x] = value;
        }
    }

    static void line(std::vector<uint32_t>& bitmap, int width, int height,
                     POINT start, POINT end, uint32_t value) {
        const int dx = std::abs(end.x - start.x);
        const int sx = start.x < end.x ? 1 : -1;
        const int dy = -std::abs(end.y - start.y);
        const int sy = start.y < end.y ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            putPixel(bitmap, width, height, start.x, start.y, value);
            if (start.x == end.x && start.y == end.y) break;
            const int doubled = 2 * error;
            if (doubled >= dy) { error += dy; start.x += sx; }
            if (doubled <= dx) { error += dx; start.y += sy; }
        }
    }

    void paint(HWND window) const {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right;
        const int height = client.bottom;
        if (width <= 0 || height <= 0) {
            EndPaint(window, &paint);
            return;
        }

        std::vector<uint32_t> bitmap(static_cast<size_t>(width) * height, 0x0006090c);
        constexpr int margin = 72;
        const int size = std::max(1, std::min(width, height) - margin * 2);
        const int left = (width - size) / 2;
        const int bottom = (height + size) / 2;
        const float sinYaw = std::sin(yaw_);
        const float cosYaw = std::cos(yaw_);
        const float sinPitch = std::sin(pitch_);
        const float cosPitch = std::cos(pitch_);
        for (const Sample& sample : samples_) {
            const POINT point = screenPoint(sample.first, sample.second, sample.vertical,
                                            sinYaw, cosYaw, sinPitch, cosPitch,
                                            size, left, bottom);
            const uint32_t value = color(sample);
            putPixel(bitmap, width, height, point.x, point.y, value);
            putPixel(bitmap, width, height, point.x + 1, point.y, value);
            putPixel(bitmap, width, height, point.x, point.y + 1, value);
        }

        POINT vertices[8]{};
        for (int vertex = 0; vertex < 8; ++vertex) {
            vertices[vertex] = screenPoint(static_cast<float>((vertex >> 0) & 1),
                                           static_cast<float>((vertex >> 1) & 1),
                                           static_cast<float>((vertex >> 2) & 1),
                                           sinYaw, cosYaw, sinPitch, cosPitch,
                                           size, left, bottom);
        }
        for (int vertex = 0; vertex < 8; ++vertex) {
            for (int axis = 0; axis < 3; ++axis) {
                const int other = vertex ^ (1 << axis);
                if (vertex < other) {
                    line(bitmap, width, height, vertices[vertex], vertices[other], 0x00465a64);
                }
            }
        }
        line(bitmap, width, height, vertices[0], vertices[1], 0x00ff4b4b);
        line(bitmap, width, height, vertices[0], vertices[2], 0x004bdc78);
        line(bitmap, width, height, vertices[0], vertices[4], 0x004b82ff);

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, 0, 0, width, height, 0, 0, width, height, bitmap.data(), &info,
                      DIB_RGB_COLORS, SRCCOPY);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        SetBkColor(dc, RGB(6, 9, 12));
        SetBkMode(dc, OPAQUE);
        const CubeDescription& description = cubeDescription(space_);
        const auto drawLabel = [&](const wchar_t* name, float value, POINT point,
                                   int offsetX, int offsetY, COLORREF color) {
            wchar_t text[64]{};
            _snwprintf_s(text, _countof(text), _TRUNCATE, L"%ls %.4g", name, value);
            SIZE extent{};
            GetTextExtentPoint32W(dc, text, lstrlenW(text), &extent);
            const int x = std::clamp(static_cast<int>(point.x) + offsetX, 2,
                                     std::max(2, width - static_cast<int>(extent.cx) - 2));
            const int y = std::clamp(static_cast<int>(point.y) + offsetY, 2,
                                     std::max(2, height - static_cast<int>(extent.cy) - 2));
            SetTextColor(dc, color);
            TextOutW(dc, x, y, text, lstrlenW(text));
        };
        const int originDirection = vertices[0].y < height / 2 ? 1 : -1;
        drawLabel(description.first, ranges_.first.minimum, vertices[0],
                  4, originDirection * 4, RGB(255, 75, 75));
        drawLabel(description.second, ranges_.second.minimum, vertices[0],
                  4, originDirection * 20, RGB(75, 220, 120));
        drawLabel(description.vertical, ranges_.vertical.minimum, vertices[0],
                  4, originDirection * 36, RGB(75, 130, 255));
        drawLabel(description.first, ranges_.first.maximum, vertices[1],
                  4, -8, RGB(255, 75, 75));
        drawLabel(description.second, ranges_.second.maximum, vertices[2],
                  4, -8, RGB(75, 220, 120));
        drawLabel(description.vertical, ranges_.vertical.maximum, vertices[4],
                  4, -8, RGB(75, 130, 255));
        EndPaint(window, &paint);
    }

    HWND window_ = nullptr;
    CubeSpace space_ = CubeSpace::rgb;
    CubeRanges ranges_ = cubeRanges(CubeSpace::rgb);
    bool dragging_ = false;
    POINTS dragPoint_{};
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    std::vector<Sample> samples_;
};
