#include "ui/Window.hpp"
#include <commctrl.h>
#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Native dialogs use English when the corresponding Windows resources are installed.
    ULONG languageCount{};
    SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, L"en-US\0", &languageCount);
    SetThreadUILanguage(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = 1;
    try {
        offsetdiff::ui::Window window;
        result = window.run(instance, show);
    } catch (const std::exception& e) {
        const auto message = offsetdiff::ui::wide(e.what());
        MessageBoxW(nullptr, message.c_str(), L"xeil / offsetdiff — Startup error", MB_ICONERROR);
    }
    if (SUCCEEDED(hr))
        CoUninitialize();
    return result;
}
