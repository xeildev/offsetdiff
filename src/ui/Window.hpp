#pragma once
#include "Renderer.hpp"
#include "core/Comparator.hpp"
#include "core/Exporter.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace offsetdiff::ui {
struct Model {
    Dump oldDump, newDump;
    bool hasOld{}, hasNew{};
    Comparison comparison;
};
struct View {
    std::shared_ptr<const Model> model;
    std::vector<std::size_t> indices;
    std::wstring search;
    int filter{-1}, sort{1};
    bool ascending{true};
};
class Window {
public:
    Window() = default;
    ~Window();
    int run(HINSTANCE instance, int show);

private:
    enum class WorkKind { Load, View, Export };
    struct Work {
        WorkKind kind{};
        unsigned generation{};
        std::filesystem::path oldPath, newPath, destination;
        std::shared_ptr<const Model> model;
        std::wstring search;
        int filter{-1}, sort{1};
        bool ascending{true};
        ExportFormat format{};
    };
    struct Response {
        unsigned generation{};
        std::shared_ptr<View> view;
        std::wstring error, notice;
    };
    HWND hwnd_{}, search_{};
    HBRUSH editBrush_{};
    HFONT editFont_{};
    Renderer renderer_;
    Layout layout_;
    float dpi_{96};
    std::filesystem::path oldPath_, newPath_;
    std::shared_ptr<View> view_;
    std::size_t first_{}, selected_{static_cast<std::size_t>(-1)};
    int selectedColumn_{1}, filter_{-1}, sort_{1};
    bool ascending_{true}, busy_{}, exporting_{}, tracking_{}, dragging_{};
    float dragOffset_{};
    D2D1_POINT_2F mouse_{-1, -1};
    int hover_{-1}, previousHover_{-1};
    float hoverProgress_{1};
    ULONGLONG animationStart_{};
    std::wstring notice_;
    std::wstring savedSearch_;
    bool error_{};
    int savedWidth_{1220}, savedHeight_{820};
    bool savedMaximized_{};
    std::filesystem::path settingsPath_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Work> pending_;
    std::deque<Response> responses_;
    std::atomic<unsigned> generation_{};
    std::jthread worker_;
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    void paint();
    void resize();
    void invalidate();
    void browse(bool old);
    void request(WorkKind kind);
    void enqueue(Work work);
    void workerLoop(std::stop_token stop, HWND target);
    void receive();
    void exportDialog();
    void filterMenu();
    void contextMenu(POINT screen);
    void copyCell(int column);
    void copyText(std::wstring_view text);
    void click(D2D1_POINT_2F point);
    void scroll(long long rows);
    void clampScroll();
    int hit(D2D1_POINT_2F point) const;
    float hoverAmount(int id) const;
    void setHover(int id);
    void selectRow(D2D1_POINT_2F point);
    std::wstring cell(const Row& row, int column) const;
    void loadSettings();
    void saveSettings() noexcept;
    void searchChanged();
};
}
