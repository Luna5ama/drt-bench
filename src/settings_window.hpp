#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include "shader_settings.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class ShaderSettingsWindow {
public:
    using ChangeCallback = std::function<void(std::size_t, std::size_t)>;
    using ResetCallback = std::function<void()>;

    ~ShaderSettingsWindow() {
        if (window_) DestroyWindow(window_);
        if (font_) DeleteObject(font_);
    }

    void create(HWND owner, ChangeCallback callback, ResetCallback resetCallback) {
        callback_ = std::move(callback);
        resetCallback_ = std::move(resetCallback);
        const HINSTANCE module = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = module;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.lpszClassName = L"drt-bench-shader-settings";
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("RegisterClassW(shader settings) failed");
        }

        constexpr int width = 640;
        constexpr int height = 720;
        RECT ownerBounds{};
        RECT work{};
        GetWindowRect(owner, &ownerBounds);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int x = ownerBounds.right + 540;
        if (x + width > work.right) x = ownerBounds.left - width - 540;
        x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
        const int y = std::clamp(static_cast<int>(ownerBounds.top), static_cast<int>(work.top),
                                 static_cast<int>(work.bottom) - height);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName,
                                  L"Shader Settings", WS_CAPTION | WS_THICKFRAME | WS_VSCROLL,
                                  x, y, width, height, owner, nullptr, module, this);
        if (!window_) throw std::runtime_error("CreateWindowExW(shader settings) failed");
        updateFont();
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    }

    void setSettings(const std::vector<shader_settings::Setting>& settings) {
        settings_ = settings;
        scroll_ = 0;
        hovered_ = invalidIndex_;
        resetHovered_ = false;
        updateScrollbar();
        if (window_) {
            const std::wstring title = L"Shader Settings (" + std::to_wstring(settings_.size()) + L")";
            SetWindowTextW(window_, title.c_str());
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    bool visible() const {
        return window_ && IsWindowVisible(window_);
    }

private:
    static constexpr int headerHeight_ = 50;
    static constexpr int rowHeight_ = 68;
    static constexpr int topMargin_ = 8;

    static std::wstring wide(std::string_view text) {
        if (text.empty()) return {};
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                               nullptr, 0);
        if (length <= 0) return std::wstring(text.begin(), text.end());
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            result.data(), length);
        return result;
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ShaderSettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<ShaderSettingsWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: self->paint(); return 0;
        case WM_SIZE:
            self->updateScrollbar();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            self->updateFont(HIWORD(wparam));
            const auto* bounds = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, bounds->left, bounds->top,
                         bounds->right - bounds->left, bounds->bottom - bounds->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = 420;
            limits->ptMinTrackSize.y = 240;
            return 0;
        }
        case WM_MOUSEWHEEL:
            self->setScroll(self->scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 64);
            return 0;
        case WM_VSCROLL:
            self->scrollCommand(LOWORD(wparam));
            return 0;
        case WM_LBUTTONDOWN:
            if (self->resetButtonContains(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                if (self->hasModifiedSettings() && self->resetCallback_) self->resetCallback_();
            } else if (self->beginDrag(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                SetCapture(window);
            }
            return 0;
        case WM_MOUSEMOVE:
            self->trackMouseLeave();
            self->mouse_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (self->dragging_ != invalidIndex_ && (wparam & MK_LBUTTON)) {
                self->changeFromX(self->dragging_, GET_X_LPARAM(lparam));
            } else {
                self->updateHover(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            }
            return 0;
        case WM_LBUTTONUP:
            if (self->dragging_ != invalidIndex_) {
                self->changeFromX(self->dragging_, GET_X_LPARAM(lparam));
                self->dragging_ = invalidIndex_;
                if (GetCapture() == window) ReleaseCapture();
            }
            return 0;
        case WM_RBUTTONUP:
            self->resetAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_MOUSELEAVE:
            self->mouseTracking_ = false;
            self->hovered_ = invalidIndex_;
            self->resetHovered_ = false;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_CAPTURECHANGED: self->dragging_ = invalidIndex_; return 0;
        case WM_CLOSE: return 0;
        case WM_NCDESTROY:
            self->window_ = nullptr;
            return DefWindowProcW(window, message, wparam, lparam);
        default: return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    int contentHeight() const {
        return topMargin_ * 2 + static_cast<int>(settings_.size()) * rowHeight_;
    }

    void updateFont(UINT dpi = 0) {
        if (!dpi && window_) {
            const HDC dc = GetDC(window_);
            if (dc) {
                dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY));
                ReleaseDC(window_, dc);
            }
        }
        if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;
        const HFONT next = CreateFontW(
            -MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        if (!next) return;
        if (font_) DeleteObject(font_);
        font_ = next;
    }

    int maximumScroll() const {
        if (!window_) return 0;
        RECT client{};
        GetClientRect(window_, &client);
        return std::max(0, contentHeight() -
            std::max(0, static_cast<int>(client.bottom) - headerHeight_));
    }

    void updateScrollbar() {
        if (!window_) return;
        RECT client{};
        GetClientRect(window_, &client);
        scroll_ = std::clamp(scroll_, 0, maximumScroll());
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMin = 0;
        info.nMax = std::max(0, contentHeight() - 1);
        info.nPage = static_cast<UINT>(
            std::max(0, static_cast<int>(client.bottom) - headerHeight_));
        info.nPos = scroll_;
        SetScrollInfo(window_, SB_VERT, &info, TRUE);
    }

    void setScroll(int value) {
        const int next = std::clamp(value, 0, maximumScroll());
        if (next == scroll_) return;
        scroll_ = next;
        hovered_ = invalidIndex_;
        updateScrollbar();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void scrollCommand(int command) {
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
        GetScrollInfo(window_, SB_VERT, &info);
        switch (command) {
        case SB_LINEUP: setScroll(scroll_ - 32); break;
        case SB_LINEDOWN: setScroll(scroll_ + 32); break;
        case SB_PAGEUP: setScroll(scroll_ - 320); break;
        case SB_PAGEDOWN: setScroll(scroll_ + 320); break;
        case SB_THUMBTRACK: setScroll(info.nTrackPos); break;
        case SB_TOP: setScroll(0); break;
        case SB_BOTTOM: setScroll(maximumScroll()); break;
        default: break;
        }
    }

    int rowTop(std::size_t index) const {
        return headerHeight_ + topMargin_ +
               static_cast<int>(index) * rowHeight_ - scroll_;
    }

    RECT resetButtonRect() const {
        RECT client{};
        GetClientRect(window_, &client);
        return {std::max(24L, client.right - 118), 11, client.right - 24, 39};
    }

    bool resetButtonContains(int x, int y) const {
        const RECT button = resetButtonRect();
        return x >= button.left && x <= button.right &&
               y >= button.top && y <= button.bottom;
    }

    bool hasModifiedSettings() const {
        return std::any_of(settings_.begin(), settings_.end(), [](const auto& setting) {
            return setting.selected != setting.defaultSelected;
        });
    }

    std::size_t settingAt(int x, int y) const {
        RECT client{};
        GetClientRect(window_, &client);
        if (x < 16 || x > client.right - 16 || y < headerHeight_) return invalidIndex_;
        for (std::size_t index = 0; index < settings_.size(); ++index) {
            const int top = rowTop(index);
            if (y >= top && y < top + rowHeight_) return index;
        }
        return invalidIndex_;
    }

    std::size_t sliderAt(int x, int y) const {
        RECT client{};
        GetClientRect(window_, &client);
        if (x < 20 || x > client.right - 20 || y < headerHeight_) return invalidIndex_;
        for (std::size_t index = 0; index < settings_.size(); ++index) {
            const int top = rowTop(index);
            if (y >= top + 24 && y <= top + 58) return index;
        }
        return invalidIndex_;
    }

    void trackMouseLeave() {
        if (mouseTracking_) return;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
        if (TrackMouseEvent(&tracking)) mouseTracking_ = true;
    }

    void updateHover(int x, int y) {
        const std::size_t next = settingAt(x, y);
        const bool nextReset = resetButtonContains(x, y);
        if (next == hovered_ && nextReset == resetHovered_) return;
        hovered_ = next;
        resetHovered_ = nextReset;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void resetAt(int x, int y) {
        const std::size_t index = sliderAt(x, y);
        if (index == invalidIndex_ || index >= settings_.size()) return;
        auto& setting = settings_[index];
        if (setting.selected == setting.defaultSelected) return;
        setting.selected = setting.defaultSelected;
        if (callback_) callback_(index, setting.selected);
        InvalidateRect(window_, nullptr, FALSE);
    }

    bool beginDrag(int x, int y) {
        const std::size_t index = sliderAt(x, y);
        if (index == invalidIndex_) return false;
        dragging_ = index;
        changeFromX(index, x);
        return true;
    }

    void changeFromX(std::size_t index, int x) {
        if (index >= settings_.size() || settings_[index].values.empty()) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int left = 24;
        const int right = std::max(left + 1, static_cast<int>(client.right) - 24);
        const float normalized = std::clamp(static_cast<float>(x - left) / (right - left), 0.0f, 1.0f);
        const std::size_t selected = static_cast<std::size_t>(std::lround(
            normalized * static_cast<float>(settings_[index].values.size() - 1)));
        if (selected == settings_[index].selected) return;
        settings_[index].selected = selected;
        if (callback_) callback_(index, selected);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void paintTooltip(HDC dc, const RECT& client) const {
        if (hovered_ >= settings_.size() || dragging_ != invalidIndex_) return;
        const std::wstring comment = wide(settings_[hovered_].comment);
        if (comment.empty()) return;
        const int width = std::max(120, std::min(380, static_cast<int>(client.right) - 32));
        RECT text{0, 0, width - 20, 0};
        DrawTextW(dc, comment.c_str(), static_cast<int>(comment.size()), &text,
                  DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        const int tooltipWidth = text.right - text.left + 20;
        const int tooltipHeight = text.bottom - text.top + 16;
        int left = mouse_.x + 14;
        int top = mouse_.y + 18;
        left = std::clamp(left, 8, std::max(8, static_cast<int>(client.right) - tooltipWidth - 8));
        if (top + tooltipHeight > client.bottom - 8) top = mouse_.y - tooltipHeight - 12;
        top = std::max(8, top);
        RECT popup{left, top, left + tooltipWidth, top + tooltipHeight};
        const HBRUSH fill = CreateSolidBrush(RGB(20, 27, 32));
        FillRect(dc, &popup, fill);
        DeleteObject(fill);
        const HPEN border = CreatePen(PS_SOLID, 1, RGB(74, 96, 108));
        const HPEN previousPen = static_cast<HPEN>(SelectObject(dc, border));
        const HBRUSH previousBrush = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
        Rectangle(dc, popup.left, popup.top, popup.right, popup.bottom);
        SelectObject(dc, previousBrush);
        SelectObject(dc, previousPen);
        DeleteObject(border);
        SetTextColor(dc, RGB(215, 224, 230));
        RECT popupText{popup.left + 10, popup.top + 8, popup.right - 10, popup.bottom - 8};
        DrawTextW(dc, comment.c_str(), static_cast<int>(comment.size()), &popupText,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }

    void paint() const {
        PAINTSTRUCT paintStruct{};
        const HDC dc = BeginPaint(window_, &paintStruct);
        RECT client{};
        GetClientRect(window_, &client);
        const HBRUSH background = CreateSolidBrush(RGB(6, 9, 12));
        FillRect(dc, &client, background);
        DeleteObject(background);
        const HFONT previousFont = static_cast<HFONT>(
            SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT)));
        SetBkMode(dc, TRANSPARENT);

        const bool resetEnabled = hasModifiedSettings();
        const RECT resetButton = resetButtonRect();
        const COLORREF buttonColor = !resetEnabled ? RGB(35, 43, 48) :
            resetHovered_ ? RGB(60, 132, 184) : RGB(42, 91, 126);
        const HBRUSH buttonBrush = CreateSolidBrush(buttonColor);
        FillRect(dc, &resetButton, buttonBrush);
        DeleteObject(buttonBrush);
        SetTextColor(dc, resetEnabled ? RGB(235, 242, 246) : RGB(105, 118, 126));
        RECT buttonText = resetButton;
        DrawTextW(dc, L"Reset All", -1, &buttonText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        const HPEN headerDivider = CreatePen(PS_SOLID, 1, RGB(34, 45, 52));
        const HPEN oldHeaderPen = static_cast<HPEN>(SelectObject(dc, headerDivider));
        MoveToEx(dc, 16, headerHeight_ - 1, nullptr);
        LineTo(dc, client.right - 16, headerHeight_ - 1);
        SelectObject(dc, oldHeaderPen);
        DeleteObject(headerDivider);

        if (settings_.empty()) {
            SetTextColor(dc, RGB(160, 174, 184));
            RECT message{24, headerHeight_ + 16, client.right - 24, client.bottom - 24};
            DrawTextW(dc, L"No Iris-style numeric settings found in main.glsl.", -1, &message,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, previousFont);
            EndPaint(window_, &paintStruct);
            return;
        }

        const int savedDc = SaveDC(dc);
        IntersectClipRect(dc, 0, headerHeight_, client.right, client.bottom);
        for (std::size_t index = 0; index < settings_.size(); ++index) {
            const shader_settings::Setting& setting = settings_[index];
            const int top = rowTop(index);
            if (top + rowHeight_ < 0 || top > client.bottom) continue;
            const std::wstring label = wide(setting.label);
            const std::wstring value = wide(shader_settings::displayValue(setting));

            SetTextColor(dc, RGB(230, 236, 240));
            RECT labelRect{24, top + 4, client.right - 160, top + 24};
            DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &labelRect,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SetTextColor(dc, RGB(114, 192, 255));
            RECT valueRect{client.right - 150, top + 4, client.right - 24, top + 24};
            DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &valueRect,
                      DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            const int left = 24;
            const int right = client.right - 24;
            const int barY = top + 42;
            const HBRUSH rail = CreateSolidBrush(RGB(53, 67, 76));
            RECT railRect{left, barY - 2, right, barY + 3};
            FillRect(dc, &railRect, rail);
            DeleteObject(rail);
            const float normalized = setting.values.size() <= 1 ? 0.0f :
                static_cast<float>(setting.selected) / static_cast<float>(setting.values.size() - 1);
            const int knobX = left + static_cast<int>(std::lround(normalized * (right - left)));
            const HBRUSH active = CreateSolidBrush(RGB(70, 151, 214));
            RECT activeRect{left, barY - 2, knobX, barY + 3};
            FillRect(dc, &activeRect, active);
            const HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, active));
            const HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 205, 255));
            const HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
            Ellipse(dc, knobX - 7, barY - 7, knobX + 8, barY + 8);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(pen);
            DeleteObject(active);

            const HPEN divider = CreatePen(PS_SOLID, 1, RGB(34, 45, 52));
            const HPEN previous = static_cast<HPEN>(SelectObject(dc, divider));
            MoveToEx(dc, 16, top + rowHeight_ - 1, nullptr);
            LineTo(dc, client.right - 16, top + rowHeight_ - 1);
            SelectObject(dc, previous);
            DeleteObject(divider);
        }
        RestoreDC(dc, savedDc);
        paintTooltip(dc, client);
        SelectObject(dc, previousFont);
        EndPaint(window_, &paintStruct);
    }

    static constexpr std::size_t invalidIndex_ = static_cast<std::size_t>(-1);
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    ChangeCallback callback_;
    ResetCallback resetCallback_;
    std::vector<shader_settings::Setting> settings_;
    std::size_t dragging_ = invalidIndex_;
    std::size_t hovered_ = invalidIndex_;
    POINT mouse_{};
    bool mouseTracking_ = false;
    bool resetHovered_ = false;
    int scroll_ = 0;
};
