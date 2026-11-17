#include "Renderer.hpp"
#include <climits>
#include <stdexcept>

namespace offsetdiff::ui {
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Direct2D/DirectWrite initialization failed.");
}
}
std::wstring wide(std::string_view s) {
    if (s.empty())
        return {};
    const auto len = static_cast<int>(std::min<std::size_t>(s.size(), INT_MAX));
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), len, nullptr, 0);
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), len, result.data(), n);
    return result;
}
std::string utf8(std::wstring_view s) {
    if (s.empty())
        return {};
    const int len = static_cast<int>(std::min<std::size_t>(s.size(), INT_MAX));
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), len, nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), len, result.data(), n, nullptr, nullptr);
    return result;
}
void Renderer::initialize(HWND hwnd, float dpi) {
    hwnd_ = hwnd;
    dpi_ = dpi;
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()));
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(write_.GetAddressOf())));
    const float sizes[] = {13, 11, 12, 26, 26, 15};
    for (unsigned i = 0; i < 6; ++i) {
        const auto weight = (i == 3 || i == 4 || i == 5) ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
        check(write_->CreateTextFormat(i == 2 ? L"Consolas" : L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, sizes[i], L"en-US", &formats_[i]));
        formats_[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        formats_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    check(write_->CreateEllipsisTrimmingSign(formats_[0].Get(), &ellipsis_));
    createTarget();
}
void Renderer::createTarget() {
    RECT r{};
    GetClientRect(hwnd_, &r);
    auto properties = D2D1::RenderTargetProperties();
    properties.dpiX = properties.dpiY = dpi_;
    check(factory_->CreateHwndRenderTarget(
        properties, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(r.right, r.bottom)), &target_));
    check(target_->CreateSolidColorBrush(theme::text, &brush_));
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
}
void Renderer::resize(unsigned w, unsigned h) {
    if (target_ && FAILED(target_->Resize(D2D1::SizeU(w, h)))) {
        brush_.Reset();
        target_.Reset();
    }
}
void Renderer::dpi(float value) {
    dpi_ = value;
    if (target_)
        target_->SetDpi(value, value);
}
void Renderer::begin() {
    if (!target_)
        createTarget();
    target_->BeginDraw();
    target_->Clear(theme::background);
}
void Renderer::end() {
    const auto hr = target_->EndDraw();
    if (FAILED(hr)) {
        brush_.Reset();
        target_.Reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
void Renderer::panel(Rect r, D2D1_COLOR_F c, float radius, bool border) {
    brush_->SetColor(c);
    target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
    if (border) {
        brush_->SetColor(theme::border);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get(), 1);
    }
}
void Renderer::fill(Rect r, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    target_->FillRectangle(r, brush_.Get());
}
void Renderer::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width) {
    brush_->SetColor(c);
    target_->DrawLine({x1, y1}, {x2, y2}, brush_.Get(), width);
}
void Renderer::text(std::wstring_view s, Rect rect, D2D1_COLOR_F c, Font font, DWRITE_TEXT_ALIGNMENT align) {
    if (s.empty() || rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    ComPtr<IDWriteTextLayout> layout;
    auto* format = formats_[static_cast<unsigned>(font)].Get();
    // Extremely long keys must not make a visible row expensive to lay out.
    // Copying/exporting still uses the complete original string.
    if (FAILED(write_->CreateTextLayout(s.data(), static_cast<UINT32>(std::min<std::size_t>(s.size(), 4096)), format,
                                        rect.right - rect.left, rect.bottom - rect.top, &layout)))
        return;
    layout->SetTextAlignment(align);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trim, ellipsis_.Get());
    brush_->SetColor(c);
    target_->DrawTextLayout({rect.left, rect.top}, layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Renderer::clip(Rect r) {
    target_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}
void Renderer::unclip() {
    target_->PopAxisAlignedClip();
}
}
