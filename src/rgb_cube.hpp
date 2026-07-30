#pragma once

#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct RgbCubePoint {
    float x;
    float y;
};

inline RgbCubePoint projectRgbCube(float red, float green, float blue) {
    constexpr float halfHorizontal = 0.4330127f;
    const float rotatedRed = 1.0f - green;
    const float rotatedGreen = red;
    return {0.5f + (rotatedGreen - rotatedRed) * halfHorizontal,
            (0.5f * rotatedRed + 0.5f * rotatedGreen + blue) / 2.0f};
}

class RgbCubeWindow {
public:
    ~RgbCubeWindow() {
        if (window_) DestroyWindow(window_);
    }

    void create(HWND owner) {
        const HINSTANCE module = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = module;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"drt-bench-rgb-cube";
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

        constexpr int width = 500;
        constexpr int height = 500;
        RECT ownerBounds{};
        RECT work{};
        GetWindowRect(owner, &ownerBounds);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int x = ownerBounds.right + 12;
        if (x + width > work.right) x = ownerBounds.left - width - 12;
        x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
        const int y = std::clamp(static_cast<int>(ownerBounds.top), static_cast<int>(work.top),
                                 static_cast<int>(work.bottom) - height);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"Output RGB Cube",
                                  WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
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

    void update(const void* pixels, uint32_t width, uint32_t height, VkFormat format) {
        if (!window_ || !pixels || !width || !height || !supports(format)) return;
        constexpr size_t maxSamples = 100'000;
        const size_t pixelCount = static_cast<size_t>(width) * height;
        const size_t step = std::max<size_t>(1, static_cast<size_t>(
            std::ceil(std::sqrt(static_cast<double>(pixelCount) / maxSamples))));
        samples_.clear();
        samples_.reserve(std::min(maxSamples, (pixelCount + step * step - 1) / (step * step)));
        const auto* bytes = static_cast<const uint8_t*>(pixels);
        const auto* packed = static_cast<const uint32_t*>(pixels);
        for (uint32_t y = 0; y < height; y += static_cast<uint32_t>(step)) {
            for (uint32_t x = 0; x < width; x += static_cast<uint32_t>(step)) {
                const size_t index = static_cast<size_t>(y) * width + x;
                Sample sample{};
                if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                    const uint32_t value = packed[index];
                    sample.red = static_cast<uint8_t>(((value >> 0) & 1023) * 255 / 1023);
                    sample.green = static_cast<uint8_t>(((value >> 10) & 1023) * 255 / 1023);
                    sample.blue = static_cast<uint8_t>(((value >> 20) & 1023) * 255 / 1023);
                } else {
                    const size_t offset = index * 4;
                    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
                    sample.red = bytes[offset + (bgra ? 2 : 0)];
                    sample.green = bytes[offset + 1];
                    sample.blue = bytes[offset + (bgra ? 0 : 2)];
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
    };

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<RgbCubeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<RgbCubeWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: self->paint(window); return 0;
        case WM_SIZE: InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_CLOSE: DestroyWindow(window); return 0;
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

    static POINT screenPoint(float red, float green, float blue, int size, int left, int bottom) {
        const RgbCubePoint point = projectRgbCube(red, green, blue);
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
        constexpr int margin = 34;
        const int size = std::max(1, std::min(width, height) - margin * 2);
        const int left = (width - size) / 2;
        const int bottom = (height + size) / 2;
        for (const Sample& sample : samples_) {
            const POINT point = screenPoint(sample.red / 255.0f, sample.green / 255.0f,
                                            sample.blue / 255.0f, size, left, bottom);
            const uint32_t value = color(sample);
            putPixel(bitmap, width, height, point.x, point.y, value);
            putPixel(bitmap, width, height, point.x + 1, point.y, value);
            putPixel(bitmap, width, height, point.x, point.y + 1, value);
        }

        POINT vertices[8]{};
        for (int vertex = 0; vertex < 8; ++vertex) {
            vertices[vertex] = screenPoint(static_cast<float>((vertex >> 0) & 1),
                                           static_cast<float>((vertex >> 1) & 1),
                                           static_cast<float>((vertex >> 2) & 1), size, left, bottom);
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
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 75, 75));
        TextOutW(dc, vertices[1].x + 4, vertices[1].y - 8, L"R", 1);
        SetTextColor(dc, RGB(75, 220, 120));
        TextOutW(dc, vertices[2].x + 4, vertices[2].y - 8, L"G", 1);
        SetTextColor(dc, RGB(75, 130, 255));
        TextOutW(dc, vertices[4].x + 4, vertices[4].y - 8, L"B", 1);
        EndPaint(window, &paint);
    }

    HWND window_ = nullptr;
    std::vector<Sample> samples_;
};
