#pragma once
#include "Theme.hpp"
#include "Widgets.hpp"
#include <dwrite.h>
#include <string>
#include <string_view>
#include <windows.h>
#include <wrl/client.h>

namespace offsetdiff::ui {
template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
enum class Font : unsigned { Body, Small, Mono, Title, Number, Brand };
class Renderer {
public:
    void initialize(HWND hwnd, float dpi);
    void resize(unsigned width, unsigned height);
    void dpi(float dpi);
    void begin();
    void end();
    void panel(Rect rect, D2D1_COLOR_F color, float radius = 8.f, bool border = true);
    void fill(Rect rect, D2D1_COLOR_F color);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width = 1.f);
    void text(std::wstring_view text, Rect rect, D2D1_COLOR_F color = theme::text, Font font = Font::Body,
              DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    void clip(Rect rect);
    void unclip();

private:
    HWND hwnd_{};
    float dpi_{96};
    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<IDWriteTextFormat> formats_[6];
    ComPtr<IDWriteInlineObject> ellipsis_;
    void createTarget();
};
std::wstring wide(std::string_view utf8);
std::string utf8(std::wstring_view text);
}
