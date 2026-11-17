#include "Window.hpp"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>
#include <dwmapi.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <windowsx.h>

namespace offsetdiff::ui {
namespace {
constexpr UINT WorkDone = WM_APP + 1;
constexpr UINT SearchTimer = 1, HoverTimer = 2;
constexpr int SearchId = 100;
constexpr std::size_t NoSelection = static_cast<std::size_t>(-1);
std::wstring lower(std::wstring text) {
    if (!text.empty())
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}
std::wstring editText(HWND hwnd) {
    const int size = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(size) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), size + 1);
    text.resize(size);
    return text;
}
std::wstring fileSize(std::uintmax_t bytes) {
    if (bytes < 1024)
        return std::to_wstring(bytes) + L" B";
    std::wostringstream s;
    s.precision(1);
    s << std::fixed;
    if (bytes < 1024 * 1024)
        s << bytes / 1024.0 << L" KB";
    else
        s << bytes / (1024.0 * 1024) << L" MB";
    return s.str();
}
D2D1_COLOR_F blend(D2D1_COLOR_F a, D2D1_COLOR_F b, float p) {
    return {a.r + (b.r - a.r) * p, a.g + (b.g - a.g) * p, a.b + (b.b - a.b) * p, 1};
}
std::wstring statusLabel(int filter) {
    constexpr const wchar_t* names[] = {L"Changed", L"Added", L"Removed", L"Unchanged"};
    return filter < 0 ? L"All statuses" : names[filter];
}
int compareValue(const std::optional<Value>& a, const std::optional<Value>& b) {
    if (!a || !b)
        return a ? 1 : b ? -1 : 0;
    if (a->negative != b->negative)
        return a->negative ? -1 : 1;
    const int result = a->magnitude < b->magnitude ? -1 : a->magnitude > b->magnitude ? 1 : 0;
    return a->negative ? -result : result;
}
// Signed hex deltas have up to 17 digits; compare sign, digit count, then digits.
int compareDelta(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty())
        return a.empty() ? (b.empty() ? 0 : -1) : 1;
    const bool an = a.front() == '-', bn = b.front() == '-';
    if (an != bn)
        return an ? -1 : 1;
    a.remove_prefix(a.front() == '+' || an ? 3 : 2);
    b.remove_prefix(b.front() == '+' || bn ? 3 : 2);
    const int r = a.size() != b.size() ? (a.size() < b.size() ? -1 : 1) : (a < b ? -1 : a > b ? 1 : 0);
    return an ? -r : r;
}
}
Window::~Window() {
    if (worker_.joinable()) {
        worker_.request_stop();
        ++generation_;
        condition_.notify_all();
        worker_.join();
    }
    if (hwnd_ && IsWindow(hwnd_))
        DestroyWindow(hwnd_);
    if (editFont_)
        DeleteObject(editFont_);
    if (editBrush_)
        DeleteObject(editBrush_);
}
int Window::run(HINSTANCE instance, int show) {
    loadSettings();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = procedure;
    wc.lpszClassName = L"xeil.offsetdiff.Window";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.style = CS_DBLCLKS;
    if (!RegisterClassExW(&wc))
        throw std::runtime_error("Cannot register the application window.");
    const UINT initialDpi = GetDpiForSystem();
    const int w = MulDiv(savedWidth_, initialDpi, 96), h = MulDiv(savedHeight_, initialDpi, 96);
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"xeil / offsetdiff",
                            WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, instance, this);
    if (!hwnd_)
        throw std::runtime_error("Cannot create the application window.");
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    renderer_.initialize(hwnd_, dpi_);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    const DWORD corner = 2;
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));
    editBrush_ = CreateSolidBrush(RGB(23, 23, 27));
    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(SearchId)), instance, nullptr);
    if (!search_ || !editBrush_)
        throw std::runtime_error("Cannot create the search control.");
    SendMessageW(search_, EM_SETLIMITTEXT, 2048, 0);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search offsets or values…"));
    SetWindowTextW(search_, savedSearch_.c_str());
    resize();
    DragAcceptFiles(hwnd_, TRUE);
    worker_ = std::jthread([this, target = hwnd_](std::stop_token stop) { workerLoop(stop, target); });
    ShowWindow(hwnd_, savedMaximized_ ? SW_MAXIMIZE : show);
    UpdateWindow(hwnd_);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (control && msg.wParam == 'F') {
                SetFocus(search_);
                SendMessageW(search_, EM_SETSEL, 0, -1);
                continue;
            }
            if (control && msg.wParam == 'O') {
                browse((GetKeyState(VK_SHIFT) & 0x8000) == 0);
                continue;
            }
            if (control && msg.wParam == 'E') {
                exportDialog();
                continue;
            }
            if (msg.wParam == VK_ESCAPE && msg.hwnd == search_) {
                SetWindowTextW(search_, L"");
                SetFocus(hwnd_);
                continue;
            }
            if (msg.wParam == VK_TAB) {
                SetFocus(GetFocus() == search_ ? hwnd_ : search_);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
LRESULT CALLBACK Window::procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(hwnd, msg, wp, lp);
    try {
        return self->message(msg, wp, lp);
    } catch (const std::exception& e) {
        if (msg == WM_PAINT)
            ValidateRect(hwnd, nullptr);
        MessageBoxW(hwnd, wide(e.what()).c_str(), L"xeil / offsetdiff — Error", MB_ICONERROR);
        return 0;
    }
}
LRESULT Window::message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp)
            return 0;
        break;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &p);
        RECT r{};
        GetClientRect(hwnd_, &r);
        const int edge = static_cast<int>(7 * dpi_ / 96);
        if (!IsZoomed(hwnd_)) {
            const bool left = p.x < edge, right = p.x >= r.right - edge;
            const bool top = p.y < edge, bottom = p.y >= r.bottom - edge;
            if (top && left)
                return HTTOPLEFT;
            if (top && right)
                return HTTOPRIGHT;
            if (bottom && left)
                return HTBOTTOMLEFT;
            if (bottom && right)
                return HTBOTTOMRIGHT;
            if (top)
                return HTTOP;
            if (bottom)
                return HTBOTTOM;
            if (left)
                return HTLEFT;
            if (right)
                return HTRIGHT;
        }
        const D2D1_POINT_2F dp{p.x * 96 / dpi_, p.y * 96 / dpi_};
        if (dp.y < 56 && hit(dp) < 0)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        const auto monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(monitor, &mi);
        info->ptMaxPosition = {mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top};
        info->ptMaxSize = {mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
        const auto dpi = GetDpiForWindow(hwnd_);
        info->ptMinTrackSize = {MulDiv(1000, dpi ? dpi : 96, 96), MulDiv(680, dpi ? dpi : 96, 96)};
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        try {
            paint();
        } catch (...) {
            EndPaint(hwnd_, &ps);
            throw;
        }
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            renderer_.resize(LOWORD(lp), HIWORD(lp));
            resize();
            invalidate();
        }
        return 0;
    case WM_DPICHANGED: {
        dpi_ = static_cast<float>(HIWORD(wp));
        renderer_.dpi(dpi_);
        const auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        resize();
        invalidate();
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        auto dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(245, 245, 245));
        SetBkColor(dc, RGB(23, 23, 27));
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == SearchId && HIWORD(wp) == EN_CHANGE) {
            searchChanged();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == SearchTimer) {
            KillTimer(hwnd_, SearchTimer);
            request(WorkKind::View);
        } else if (wp == HoverTimer) {
            hoverProgress_ = std::min(1.f, static_cast<float>(GetTickCount64() - animationStart_) / 110);
            if (hoverProgress_ >= 1) {
                KillTimer(hwnd_, HoverTimer);
                previousHover_ = -1;
            }
            invalidate();
        }
        return 0;
    case WM_MOUSEMOVE: {
        const auto oldMouse = mouse_;
        mouse_ = {GET_X_LPARAM(lp) * 96 / dpi_, GET_Y_LPARAM(lp) * 96 / dpi_};
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
            tracking_ = true;
        }
        if (dragging_ && view_) {
            const auto thumb = layout_.thumb(view_->indices.size(), first_);
            const float range = (layout_.track.bottom - layout_.track.top) - (thumb.bottom - thumb.top);
            const auto maxFirst = view_->indices.size() > static_cast<std::size_t>(layout_.visibleRows())
                                      ? view_->indices.size() - layout_.visibleRows()
                                      : 0;
            const float ratio =
                range > 0 ? std::clamp((mouse_.y - dragOffset_ - layout_.track.top) / range, 0.f, 1.f) : 0;
            first_ = static_cast<std::size_t>(std::round(ratio * static_cast<double>(maxFirst)));
            invalidate();
        } else {
            setHover(hit(mouse_));
            if (contains(layout_.body, mouse_) || contains(layout_.body, oldMouse)) {
                const auto previousRow = contains(layout_.body, oldMouse)
                                             ? static_cast<int>((oldMouse.y - layout_.body.top) / Layout::rowHeight)
                                             : -1;
                const auto nextRow = contains(layout_.body, mouse_)
                                         ? static_cast<int>((mouse_.y - layout_.body.top) / Layout::rowHeight)
                                         : -1;
                if (previousRow != nextRow)
                    invalidate();
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        mouse_ = {-1, -1};
        setHover(-1);
        invalidate();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && hover_ >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        const D2D1_POINT_2F p{GET_X_LPARAM(lp) * 96 / dpi_, GET_Y_LPARAM(lp) * 96 / dpi_};
        if (contains(layout_.track, p) && view_ &&
            view_->indices.size() > static_cast<std::size_t>(layout_.visibleRows())) {
            const auto thumb = layout_.thumb(view_->indices.size(), first_);
            if (contains(thumb, p)) {
                dragging_ = true;
                dragOffset_ = p.y - thumb.top;
                SetCapture(hwnd_);
            } else
                scroll((p.y < thumb.top ? -1 : 1) * layout_.visibleRows());
        } else
            click(p);
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return 0;
    case WM_LBUTTONDBLCLK: {
        const D2D1_POINT_2F p{GET_X_LPARAM(lp) * 96 / dpi_, GET_Y_LPARAM(lp) * 96 / dpi_};
        if (contains(layout_.body, p)) {
            selectRow(p);
            copyCell(selectedColumn_);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        static int remainder{};
        remainder += GET_WHEEL_DELTA_WPARAM(wp);
        const int steps = remainder / WHEEL_DELTA;
        remainder %= WHEEL_DELTA;
        scroll(-static_cast<long long>(steps) * (lines == WHEEL_PAGESCROLL ? layout_.visibleRows() : lines));
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (screen.x == -1 && screen.y == -1) {
            screen = {static_cast<LONG>(layout_.body.left * dpi_ / 96 + 50),
                      static_cast<LONG>(layout_.body.top * dpi_ / 96 + 20)};
            ClientToScreen(hwnd_, &screen);
        } else {
            POINT p = screen;
            ScreenToClient(hwnd_, &p);
            selectRow({p.x * 96 / dpi_, p.y * 96 / dpi_});
        }
        contextMenu(screen);
        return 0;
    }
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'C') {
            copyCell(selectedColumn_);
            return 0;
        }
        if (wp == VK_F5) {
            if (!oldPath_.empty() || !newPath_.empty())
                request(WorkKind::Load);
            return 0;
        }
        if (view_ && !view_->indices.empty()) {
            if (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT || wp == VK_HOME || wp == VK_END) {
                long long row = selected_ == NoSelection ? 0 : static_cast<long long>(selected_);
                if (wp == VK_UP)
                    --row;
                if (wp == VK_DOWN)
                    ++row;
                if (wp == VK_PRIOR)
                    row -= layout_.visibleRows();
                if (wp == VK_NEXT)
                    row += layout_.visibleRows();
                if (wp == VK_HOME)
                    row = 0;
                if (wp == VK_END)
                    row = static_cast<long long>(view_->indices.size()) - 1;
                selected_ =
                    static_cast<std::size_t>(std::clamp(row, 0LL, static_cast<long long>(view_->indices.size()) - 1));
                if (selected_ < first_)
                    first_ = selected_;
                if (selected_ >= first_ + layout_.visibleRows())
                    first_ = selected_ - layout_.visibleRows() + 1;
                clampScroll();
                invalidate();
                return 0;
            }
            if (wp == VK_LEFT || wp == VK_RIGHT) {
                selectedColumn_ = std::clamp(selectedColumn_ + (wp == VK_LEFT ? -1 : 1), 0, 4);
                invalidate();
                return 0;
            }
        }
        break;
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wp);
        struct DropGuard {
            HDROP drop;
            ~DropGuard() {
                DragFinish(drop);
            }
        } guard{drop};
        const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (!count)
            return 0;
        std::vector<std::filesystem::path> paths;
        for (UINT i = 0; i < std::min(count, 2u); ++i) {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size()));
            path.pop_back();
            paths.emplace_back(path);
        }
        POINT p{};
        DragQueryPoint(drop, &p);
        if (paths.size() == 2) {
            oldPath_ = paths[0];
            newPath_ = paths[1];
        } else if (contains(layout_.newFile, {p.x * 96 / dpi_, p.y * 96 / dpi_}) ||
                   (!oldPath_.empty() && newPath_.empty() &&
                    !contains(layout_.oldFile, {p.x * 96 / dpi_, p.y * 96 / dpi_})))
            newPath_ = paths[0];
        else
            oldPath_ = paths[0];
        request(WorkKind::Load);
        return 0;
    }
    case WorkDone:
        receive();
        return 0;
    case WM_CLOSE:
        saveSettings();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        worker_.request_stop();
        ++generation_;
        condition_.notify_all();
        KillTimer(hwnd_, SearchTimer);
        KillTimer(hwnd_, HoverTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
void Window::resize() {
    RECT r{};
    GetClientRect(hwnd_, &r);
    layout_.update(r.right * 96 / dpi_, r.bottom * 96 / dpi_);
    if (search_) {
        const auto rect = layout_.search;
        MoveWindow(search_, static_cast<int>((rect.left + 37) * dpi_ / 96),
                   static_cast<int>((rect.top + 9) * dpi_ / 96),
                   static_cast<int>((rect.right - rect.left - 48) * dpi_ / 96), static_cast<int>(20 * dpi_ / 96), TRUE);
        const int fontHeight = -static_cast<int>(std::round(13 * dpi_ / 96));
        LOGFONTW old{};
        if (!editFont_ || !GetObjectW(editFont_, sizeof(old), &old) || old.lfHeight != fontHeight) {
            auto next =
                CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            if (next) {
                SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
                if (editFont_)
                    DeleteObject(editFont_);
                editFont_ = next;
            }
        }
    }
    clampScroll();
}
void Window::invalidate() {
    InvalidateRect(hwnd_, nullptr, FALSE);
}
float Window::hoverAmount(int id) const {
    if (hover_ == id)
        return hoverProgress_;
    if (previousHover_ == id)
        return 1 - hoverProgress_;
    return 0;
}
void Window::setHover(int id) {
    if (id == hover_)
        return;
    previousHover_ = hover_;
    hover_ = id;
    hoverProgress_ = 0;
    animationStart_ = GetTickCount64();
    SetTimer(hwnd_, HoverTimer, 16, nullptr);
    invalidate();
}
int Window::hit(D2D1_POINT_2F p) const {
    if (contains(layout_.oldFile, p))
        return 0;
    if (contains(layout_.newFile, p))
        return 1;
    if (contains(layout_.minimize, p))
        return 3;
    if (contains(layout_.maximize, p))
        return 4;
    if (contains(layout_.close, p))
        return 5;
    for (int i = 0; i < 4; ++i)
        if (contains(layout_.stats[i], p))
            return 6 + i;
    if (contains(layout_.filter, p))
        return 10;
    if (contains(layout_.exportButton, p))
        return 11;
    if (contains(layout_.tableHeader, p))
        return 12 + layout_.columnAt(p.x);
    return -1;
}
void Window::paint() {
    if (IsIconic(hwnd_))
        return;
    renderer_.begin();
    auto& d = renderer_;
    const float w = layout_.width, h = layout_.height;
    d.line(28, 22, 35, 34, theme::text, 2);
    d.line(37, 22, 44, 34, theme::muted, 2);
    d.text(L"xeil", {56, 0, 84, 56}, theme::text, Font::Brand);
    d.text(L"/ offsetdiff", {91, 0, 190, 56}, theme::muted, Font::Brand);
    d.panel({201, 19, 247, 37}, theme::panel, 4, false);
    d.text(L"v0.1.0", {201, 19, 247, 37}, theme::muted, Font::Small, DWRITE_TEXT_ALIGNMENT_CENTER);
    const Rect captions[] = {layout_.minimize, layout_.maximize, layout_.close};
    for (int i = 0; i < 3; ++i)
        d.fill(captions[i], blend(theme::background, i == 2 ? rgb(0x8B3339) : theme::hover, hoverAmount(3 + i)));
    const float mx = w - 120, ax = w - 72, cx = w - 24;
    d.line(mx - 5, 28, mx + 5, 28, theme::muted);
    d.line(ax - 5, 23, ax + 5, 23, theme::muted);
    d.line(ax + 5, 23, ax + 5, 33, theme::muted);
    d.line(ax + 5, 33, ax - 5, 33, theme::muted);
    d.line(ax - 5, 33, ax - 5, 23, theme::muted);
    d.line(cx - 5, 23, cx + 5, 33, theme::text);
    d.line(cx + 5, 23, cx - 5, 33, theme::text);
    d.line(0, 56, w, 56, theme::border);
    d.text(L"Compare offset dumps", {28, 78, w - 28, 114}, theme::text, Font::Title);
    const auto model = view_ ? view_->model : nullptr;
    const Rect files[] = {layout_.oldFile, layout_.newFile};
    for (int i = 0; i < 2; ++i) {
        const auto r = files[i];
        const bool loaded = model && (i == 0 ? model->hasOld : model->hasNew);
        const auto& path = i == 0 ? oldPath_ : newPath_;
        const auto* dump = loaded ? (i == 0 ? &model->oldDump : &model->newDump) : nullptr;
        d.panel(r, blend(theme::panel, theme::hover, hoverAmount(i)));
        d.text(i == 0 ? L"OLD FILE" : L"NEW FILE", {r.left + 16, r.top + 10, r.right - 110, r.top + 29}, theme::muted,
               Font::Small);
        const auto name = path.empty() ? L"Select a JSON dump" : path.filename().wstring();
        d.text(name, {r.left + 16, r.top + 34, r.right - 115, r.top + 57}, loaded ? theme::text : theme::muted);
        const auto detail = dump
                                ? fileSize(dump->bytes) + L"  ·  " + std::to_wstring(dump->entries.size()) + L" offsets"
                                : L"Drop a file here or browse";
        d.text(detail, {r.left + 16, r.top + 64, r.right - 110, r.top + 84}, theme::muted, Font::Small);
        const Rect button{r.right - 96, r.top + 35, r.right - 16, r.top + 65};
        d.panel(button, blend(theme::panel, rgb(0x303036), hoverAmount(i)), 5);
        d.text(L"Browse…", button, theme::text, Font::Body, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    constexpr const wchar_t* statNames[] = {L"Changed", L"Added", L"Removed", L"Unchanged"};
    for (int i = 0; i < 4; ++i) {
        const auto r = layout_.stats[i];
        const bool active = filter_ == i;
        d.panel(r, active ? theme::selected : blend(theme::panel, theme::hover, hoverAmount(6 + i)));
        d.text(statNames[i], {r.left + 16, r.top + 9, r.right - 16, r.top + 29}, theme::muted, Font::Small);
        d.text(model && model->hasOld && model->hasNew ? std::to_wstring(model->comparison.counts[i]) : L"—",
               {r.left + 16, r.top + 30, r.right - 16, r.bottom - 8}, theme::text, Font::Number);
        if (active)
            d.line(r.left + 17, r.bottom - 1, r.right - 17, r.bottom - 1, theme::text, 1.5f);
    }
    d.text(L"Comparison", {28, 335, 200, 360}, theme::text, Font::Brand);
    std::wstring summary;
    if (model && model->hasOld && model->hasNew) {
        summary = model->comparison.identical
                      ? L"IDENTICAL OFFSETS"
                      : std::to_wstring(model->comparison.counts[0] + model->comparison.counts[1] +
                                        model->comparison.counts[2]) +
                            L" DIFFERENCES";
    }
    d.text(summary, {220, 335, w - 28, 360}, theme::muted, Font::Small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    d.panel(layout_.search, theme::panel, 6);
    const float sx = layout_.search.left + 18, sy = layout_.search.top + 17;
    // Search glyph from a short polygonal circle, independent of icon fonts.
    constexpr float pi = 3.14159265f;
    for (int i = 0; i < 16; ++i) {
        const float a = i * pi / 8, b = (i + 1) * pi / 8;
        d.line(sx + std::cos(a) * 5, sy + std::sin(a) * 5, sx + std::cos(b) * 5, sy + std::sin(b) * 5, theme::muted);
    }
    d.line(sx + 4, sy + 4, sx + 8, sy + 8, theme::muted);
    d.panel(layout_.filter, blend(theme::panel, theme::hover, hoverAmount(10)), 6);
    auto fr = inset(layout_.filter, 12, 0);
    fr.right -= 20;
    d.text(statusLabel(filter_), fr);
    const float fx = layout_.filter.right - 18, fy = layout_.filter.top + 18;
    d.line(fx - 3, fy - 2, fx, fy + 1, theme::muted);
    d.line(fx, fy + 1, fx + 3, fy - 2, theme::muted);
    const bool canExport = model && model->hasOld && model->hasNew && !busy_;
    d.panel(layout_.exportButton, canExport ? blend(theme::text, rgb(0xCCCCD2), hoverAmount(11)) : theme::panel, 6,
            !canExport);
    d.text(L"Export  ↗", layout_.exportButton, canExport ? theme::background : theme::muted, Font::Body,
           DWRITE_TEXT_ALIGNMENT_CENTER);
    if (view_ && model->hasOld && model->hasNew) {
        d.text(std::to_wstring(view_->indices.size()) + L" / " + std::to_wstring(model->comparison.rows.size()) +
                   L" offsets",
               {536, 373, w - 162, 409}, theme::muted, Font::Small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    d.panel(layout_.table, theme::panel, 7);
    d.line(layout_.table.left, layout_.tableHeader.bottom, layout_.table.right, layout_.tableHeader.bottom,
           theme::border);
    constexpr const wchar_t* headings[] = {L"STATUS", L"OFFSET NAME", L"OLD VALUE", L"NEW VALUE", L"DIFFERENCE"};
    for (int i = 0; i < 5; ++i) {
        Rect r{layout_.columns[i] + 14, layout_.tableHeader.top, layout_.columns[i + 1] - 8,
               layout_.tableHeader.bottom};
        d.text(std::wstring(headings[i]) + (sort_ == i ? (ascending_ ? L"  ↑" : L"  ↓") : L""), r,
               sort_ == i ? theme::text : theme::muted, Font::Small);
    }
    d.clip(layout_.body);
    if (view_ && !view_->indices.empty()) {
        const auto count = view_->indices.size();
        const auto last = std::min(count, first_ + static_cast<std::size_t>(layout_.visibleRows()) + 1);
        for (auto i = first_; i < last; ++i) {
            const auto& row = model->comparison.rows[view_->indices[i]];
            const float y = layout_.body.top + static_cast<float>(i - first_) * Layout::rowHeight;
            Rect rr{layout_.body.left + 1, y, layout_.body.right, y + Layout::rowHeight};
            if (i == selected_)
                d.fill(rr, theme::selected);
            else if (contains(rr, mouse_))
                d.fill(rr, blend(theme::panel, theme::hover, .55f));
            d.line(rr.left + 12, rr.bottom, rr.right - 6, rr.bottom, rgb(0x222227));
            for (int column = 0; column < 5; ++column) {
                Rect cr{layout_.columns[column] + 14, y, layout_.columns[column + 1] - 8, y + Layout::rowHeight};
                auto color = column == 1 ? theme::text : theme::muted;
                if (column == 3 && row.status == Status::Changed)
                    color = theme::text;
                if (column == 0) {
                    const auto badge = Rect{cr.left - 2, y + 10, cr.right - 1, y + 28};
                    d.panel(badge, row.status == Status::Changed ? rgb(0x323238) : rgb(0x222227), 4, false);
                    d.text(cell(row, column), badge, row.status == Status::Unchanged ? theme::muted : theme::text,
                           Font::Small, DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    auto text = cell(row, column);
                    for (auto& c : text)
                        if (c < L' ')
                            c = L' ';
                    d.text(text, cr, color, Font::Mono);
                    if (i == selected_ && column == selectedColumn_)
                        d.line(cr.left, cr.bottom - 6, std::min(cr.right, cr.left + 22), cr.bottom - 6, theme::muted);
                }
            }
        }
    } else {
        const bool ready = model && model->hasOld && model->hasNew;
        const float y = (layout_.body.top + layout_.body.bottom) / 2;
        const auto headline =
            busy_   ? L"Processing files…"
            : ready ? (model->comparison.rows.empty() ? L"No offsets in either file" : L"No matching offsets")
                    : L"Select two JSON files to compare";
        d.text(headline, {layout_.body.left + 20, y - 16, layout_.body.right - 20, y + 16}, theme::muted, Font::Body,
               DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    d.unclip();
    if (view_ && view_->indices.size() > static_cast<std::size_t>(layout_.visibleRows()))
        d.panel(layout_.thumb(view_->indices.size(), first_), dragging_ ? rgb(0x707079) : rgb(0x45454D), 3, false);
    d.text(busy_ ? L"Processing…" : notice_, {28, h - 37, w - 28, h - 10}, error_ ? theme::red : theme::muted,
           Font::Small);
    d.end();
}
void Window::browse(bool old) {
    std::vector<wchar_t> name(32768, L'\0');
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = name.data();
    dialog.nMaxFile = static_cast<DWORD>(name.size());
    dialog.lpstrFilter = L"JSON offset dumps (*.json)\0*.json\0All files (*.*)\0*.*\0";
    dialog.lpstrTitle = old ? L"Select OLD offset dump" : L"Select NEW offset dump";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        (old ? oldPath_ : newPath_) = name.data();
        request(WorkKind::Load);
    } else if (CommDlgExtendedError())
        MessageBoxW(hwnd_, L"The file picker could not be opened.", L"offsetdiff", MB_ICONERROR);
}
void Window::request(WorkKind kind) {
    if (kind == WorkKind::View && !view_)
        return;
    // Finish an explicitly requested export before applying another view query.
    if (kind == WorkKind::View && exporting_)
        return;
    Work work;
    work.kind = kind;
    work.oldPath = oldPath_;
    work.newPath = newPath_;
    work.model = view_ ? view_->model : nullptr;
    work.search = editText(search_);
    work.filter = filter_;
    work.sort = sort_;
    work.ascending = ascending_;
    if (kind == WorkKind::Load)
        view_.reset();
    first_ = 0;
    selected_ = NoSelection;
    enqueue(std::move(work));
}
void Window::enqueue(Work work) {
    exporting_ = work.kind == WorkKind::Export;
    work.generation = ++generation_;
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(work);
    }
    busy_ = true;
    error_ = false;
    condition_.notify_one();
    invalidate();
}
void Window::workerLoop(std::stop_token stop, HWND target) {
    while (!stop.stop_requested()) {
        Work work;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return pending_.has_value() || stop.stop_requested(); });
            if (stop.stop_requested())
                break;
            work = std::move(*pending_);
            pending_.reset();
        }
        const Cancel cancel = [&] { return stop.stop_requested() || generation_.load() != work.generation; };
        Response response;
        response.generation = work.generation;
        try {
            if (work.kind == WorkKind::Export) {
                exportFile(work.destination, work.model->comparison, work.format, cancel);
                response.notice = L"Export saved: " + work.destination.filename().wstring();
            } else {
                auto model = work.model;
                if (work.kind == WorkKind::Load) {
                    auto next = std::make_shared<Model>();
                    auto read = [&](bool old) {
                        auto& dump = old ? next->oldDump : next->newDump;
                        const auto& path = old ? work.oldPath : work.newPath;
                        if (path.empty())
                            return;
                        try {
                            dump = parseFile(path, cancel);
                        } catch (const ParseError& e) {
                            throw ParseError(std::string(old ? "OLD file: " : "NEW file: ") + e.what());
                        }
                        (old ? next->hasOld : next->hasNew) = true;
                    };
                    read(true);
                    read(false);
                    if (next->hasOld && next->hasNew)
                        next->comparison = compare(next->oldDump, next->newDump, cancel);
                    model = std::move(next);
                }
                auto view = std::make_shared<View>();
                view->model = model;
                view->search = work.search;
                view->filter = work.filter;
                view->sort = work.sort;
                view->ascending = work.ascending;
                const auto query = lower(work.search);
                const auto& rows = model->comparison.rows;
                view->indices.reserve(rows.size());
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    if (cancel())
                        throw Cancelled();
                    const auto& row = rows[i];
                    if (work.filter >= 0 && static_cast<int>(row.status) != work.filter)
                        continue;
                    if (!query.empty()) {
                        const auto haystack = lower(wide(row.name + " " + std::string(statusName(row.status)) + " " +
                                                         (row.oldValue ? row.oldValue->hex() : "") + " " +
                                                         (row.newValue ? row.newValue->hex() : "") + " " + row.delta));
                        if (haystack.find(query) == std::wstring::npos)
                            continue;
                    }
                    view->indices.push_back(i);
                }
                std::sort(view->indices.begin(), view->indices.end(), [&](std::size_t a, std::size_t b) {
                    if (cancel())
                        throw Cancelled();
                    const auto& x = rows[a];
                    const auto& y = rows[b];
                    int r{};
                    switch (work.sort) {
                    case 0:
                        r = static_cast<int>(x.status) - static_cast<int>(y.status);
                        break;
                    case 1:
                        r = x.name < y.name ? -1 : x.name > y.name ? 1 : 0;
                        break;
                    case 2:
                        r = compareValue(x.oldValue, y.oldValue);
                        break;
                    case 3:
                        r = compareValue(x.newValue, y.newValue);
                        break;
                    case 4:
                        r = compareDelta(x.delta, y.delta);
                        break;
                    }
                    if (!r)
                        r = x.name < y.name ? -1 : x.name > y.name ? 1 : 0;
                    return work.ascending ? r < 0 : r > 0;
                });
                response.view = std::move(view);
            }
        } catch (const Cancelled&) {
            continue;
        } catch (const std::exception& e) {
            response.error = wide(e.what());
        }
        if (cancel())
            continue;
        {
            std::lock_guard lock(mutex_);
            responses_.push_back(std::move(response));
        }
        PostMessageW(target, WorkDone, 0, 0);
    }
}
void Window::receive() {
    std::deque<Response> responses;
    {
        std::lock_guard lock(mutex_);
        responses.swap(responses_);
    }
    for (auto& response : responses) {
        if (response.generation != generation_)
            continue;
        busy_ = false;
        exporting_ = false;
        if (!response.error.empty()) {
            notice_ = response.error;
            error_ = true;
            invalidate();
            MessageBoxW(hwnd_, response.error.c_str(), L"xeil / offsetdiff — Could not complete operation",
                        MB_OK | MB_ICONERROR);
        } else if (response.view) {
            view_ = std::move(response.view);
            first_ = 0;
            selected_ = NoSelection;
            const auto& m = *view_->model;
            notice_ = m.hasOld && m.hasNew ? (m.comparison.identical ? L"Identical offsets." : L"Comparison complete.")
                                           : L"File loaded.";
            if (view_->filter != filter_ || view_->search != editText(search_) || view_->sort != sort_ ||
                view_->ascending != ascending_)
                request(WorkKind::View);
        } else {
            notice_ = response.notice;
            if (view_ && (view_->filter != filter_ || view_->search != editText(search_) || view_->sort != sort_ ||
                          view_->ascending != ascending_))
                request(WorkKind::View);
        }
    }
    clampScroll();
    invalidate();
}
void Window::searchChanged() {
    SetTimer(hwnd_, SearchTimer, 180, nullptr);
}
void Window::exportDialog() {
    if (busy_ || !view_ || !view_->model->hasOld || !view_->model->hasNew)
        return;
    std::vector<wchar_t> filename(32768, L'\0');
    const std::wstring initial = L"offsetdiff-comparison.json";
    std::copy(initial.begin(), initial.end(), filename.begin());
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrFilter = L"JSON comparison (*.json)\0*.json\0Text comparison (*.txt)\0*.txt\0";
    dialog.lpstrDefExt = L"json";
    dialog.lpstrTitle = L"Export full comparison (all rows)";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) {
        if (CommDlgExtendedError())
            MessageBoxW(hwnd_, L"The export picker could not be opened.", L"offsetdiff", MB_ICONERROR);
        return;
    }
    auto path = std::filesystem::path(filename.data());
    const auto ext = lower(path.extension().wstring());
    auto format = ext == L".txt"    ? ExportFormat::Text
                  : ext == L".json" ? ExportFormat::Json
                                    : (dialog.nFilterIndex == 2 ? ExportFormat::Text : ExportFormat::Json);
    // Never replace either input dump with its comparison export.
    for (const auto& input : {oldPath_, newPath_}) {
        if (input.empty())
            continue;
        std::error_code ec;
        const bool same = std::filesystem::equivalent(input, path, ec);
        if (same || lower(std::filesystem::absolute(input).lexically_normal().wstring()) ==
                        lower(std::filesystem::absolute(path).lexically_normal().wstring())) {
            MessageBoxW(hwnd_, L"Choose a different export filename. The destination is one of your input dumps.",
                        L"Protect input file", MB_ICONWARNING);
            return;
        }
    }
    Work work;
    work.kind = WorkKind::Export;
    work.destination = path;
    work.format = format;
    work.model = view_->model;
    enqueue(std::move(work));
}
void Window::filterMenu() {
    const auto menu = CreatePopupMenu();
    if (!menu)
        return;
    const auto flags = MF_STRING;
    AppendMenuW(menu, flags | (filter_ == -1 ? MF_CHECKED : 0), 1, L"All statuses");
    for (int i = 0; i < 4; ++i)
        AppendMenuW(menu, flags | (filter_ == i ? MF_CHECKED : 0), i + 2, statusLabel(i).c_str());
    POINT p{static_cast<LONG>(layout_.filter.left * dpi_ / 96), static_cast<LONG>(layout_.filter.bottom * dpi_ / 96)};
    ClientToScreen(hwnd_, &p);
    const auto chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (chosen) {
        filter_ = static_cast<int>(chosen) - 2;
        request(WorkKind::View);
        invalidate();
    }
}
void Window::contextMenu(POINT screen) {
    if (!view_ || selected_ >= view_->indices.size())
        return;
    const auto menu = CreatePopupMenu();
    if (!menu)
        return;
    const wchar_t* items[] = {L"Copy selected cell\tCtrl+C",
                              L"Copy offset name",
                              L"Copy old value",
                              L"Copy new value",
                              L"Copy difference",
                              L"Copy entire row"};
    for (int i = 0; i < 6; ++i)
        AppendMenuW(menu, MF_STRING, i + 1, items[i]);
    const auto chosen =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (chosen)
        copyCell(chosen == 1 ? selectedColumn_ : chosen == 6 ? 5 : static_cast<int>(chosen) - 1);
}
std::wstring Window::cell(const Row& row, int column) const {
    switch (column) {
    case 0:
        return wide(statusName(row.status));
    case 1:
        return wide(row.name);
    case 2:
        return row.oldValue ? wide(row.oldValue->hex()) : L"—";
    case 3:
        return row.newValue ? wide(row.newValue->hex()) : L"—";
    case 4:
        return row.delta.empty() ? L"—" : wide(row.delta);
    default: {
        std::wstring text;
        for (int i = 0; i < 5; ++i) {
            if (i)
                text += L'\t';
            text += cell(row, i);
        }
        return text;
    }
    }
}
void Window::copyCell(int column) {
    if (!view_ || selected_ >= view_->indices.size())
        return;
    copyText(cell(view_->model->comparison.rows[view_->indices[selected_]], column));
}
void Window::copyText(std::wstring_view text) {
    // Allocate before opening/emptying the clipboard so allocation failure preserves its contents.
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        notice_ = L"Clipboard allocation failed.";
        error_ = true;
        invalidate();
        return;
    }
    auto* dest = GlobalLock(memory);
    if (!dest) {
        GlobalFree(memory);
        return;
    }
    memcpy(dest, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(dest)[text.size()] = L'\0';
    GlobalUnlock(memory);
    if (!OpenClipboard(hwnd_)) {
        GlobalFree(memory);
        notice_ = L"Clipboard is busy. Try again.";
        error_ = true;
        invalidate();
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        notice_ = L"Copy failed.";
        error_ = true;
    } else {
        notice_ = L"Copied to clipboard.";
        error_ = false;
    }
    CloseClipboard();
    invalidate();
}
void Window::click(D2D1_POINT_2F point) {
    const int id = hit(point);
    if (id == 0 || id == 1)
        browse(id == 0);
    else if (id == 3)
        ShowWindow(hwnd_, SW_MINIMIZE);
    else if (id == 4)
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    else if (id == 5)
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
    else if (id >= 6 && id <= 9) {
        filter_ = filter_ == id - 6 ? -1 : id - 6;
        request(WorkKind::View);
        invalidate();
    } else if (id == 10)
        filterMenu();
    else if (id == 11)
        exportDialog();
    else if (id >= 12 && id <= 16) {
        if (sort_ == id - 12)
            ascending_ = !ascending_;
        else {
            sort_ = id - 12;
            ascending_ = true;
        }
        request(WorkKind::View);
        invalidate();
    } else if (contains(layout_.body, point))
        selectRow(point);
}
void Window::selectRow(D2D1_POINT_2F p) {
    if (!contains(layout_.body, p) || !view_)
        return;
    const auto row = first_ + static_cast<std::size_t>((p.y - layout_.body.top) / Layout::rowHeight);
    if (row < view_->indices.size()) {
        selected_ = row;
        selectedColumn_ = layout_.columnAt(p.x);
        invalidate();
    }
}
void Window::scroll(long long rows) {
    if (!view_)
        return;
    const auto current = static_cast<long long>(first_);
    first_ = static_cast<std::size_t>(std::max(0LL, current + rows));
    clampScroll();
    invalidate();
}
void Window::clampScroll() {
    const auto count = view_ ? view_->indices.size() : 0;
    const auto visible = static_cast<std::size_t>(layout_.visibleRows());
    first_ = std::min(first_, count > visible ? count - visible : 0);
}
void Window::loadSettings() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        return;
    settingsPath_ = std::filesystem::path(folder) / L"xeil" / L"offsetdiff" / L"settings.json";
    CoTaskMemFree(folder);
    try {
        std::error_code ec;
        const auto size = std::filesystem::file_size(settingsPath_, ec);
        if (ec || size > 64 * 1024)
            return;
        std::ifstream in(settingsPath_);
        auto j = nlohmann::json::parse(in);
        savedWidth_ = std::clamp(j.value("width", 1220), 1000, 3840);
        savedHeight_ = std::clamp(j.value("height", 820), 680, 2160);
        savedMaximized_ = j.value("maximized", false);
        filter_ = std::clamp(j.value("filter", -1), -1, 3);
        sort_ = std::clamp(j.value("sort", 1), 0, 4);
        ascending_ = j.value("ascending", true);
        savedSearch_ = wide(j.value("search", std::string{}));
        if (savedSearch_.size() > 2048)
            savedSearch_.resize(2048);
    } catch (...) { /* Corrupt or inaccessible preferences do not prevent startup. */
    }
}
void Window::saveSettings() noexcept {
    try {
        if (settingsPath_.empty())
            return;
        WINDOWPLACEMENT p{sizeof(p)};
        GetWindowPlacement(hwnd_, &p);
        const auto dpi = GetDpiForWindow(hwnd_);
        nlohmann::json j = {{"width", MulDiv(p.rcNormalPosition.right - p.rcNormalPosition.left, 96, dpi ? dpi : 96)},
                            {"height", MulDiv(p.rcNormalPosition.bottom - p.rcNormalPosition.top, 96, dpi ? dpi : 96)},
                            {"maximized", IsZoomed(hwnd_) != FALSE || (p.flags & WPF_RESTORETOMAXIMIZED) != 0},
                            {"filter", filter_},
                            {"sort", sort_},
                            {"ascending", ascending_},
                            {"search", utf8(editText(search_))}};
        std::filesystem::create_directories(settingsPath_.parent_path());
        auto tmp = settingsPath_;
        tmp += L".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(2);
        out.close();
        if (out)
            MoveFileExW(tmp.c_str(), settingsPath_.c_str(), MOVEFILE_REPLACE_EXISTING);
    } catch (...) { /* Preferences are best effort; a read-only profile remains usable. */
    }
}
}
