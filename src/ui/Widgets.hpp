#pragma once
#include <algorithm>
#include <array>
#include <d2d1.h>

namespace offsetdiff::ui {
using Rect = D2D1_RECT_F;
inline bool contains(Rect r, D2D1_POINT_2F p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}
inline Rect inset(Rect r, float x, float y) {
    return {r.left + x, r.top + y, r.right - x, r.bottom - y};
}
struct Layout {
    float width{}, height{};
    Rect oldFile{}, newFile{}, minimize{}, maximize{}, close{};
    std::array<Rect, 4> stats{};
    Rect search{}, filter{}, exportButton{}, table{}, tableHeader{}, body{}, track{};
    std::array<float, 6> columns{};
    static constexpr float rowHeight = 38.f;
    void update(float w, float h) {
        width = w;
        height = h;
        close = {w - 48, 0, w, 56};
        maximize = {w - 96, 0, w - 48, 56};
        minimize = {w - 144, 0, w - 96, 56};
        const float half = (w - 68) / 2;
        oldFile = {28, 124, 28 + half, 224};
        newFile = {40 + half, 124, w - 28, 224};
        const float sw = (w - 92) / 4;
        for (int i = 0; i < 4; ++i)
            stats[i] = {28 + i * (sw + 12), 240, 28 + i * (sw + 12) + sw, 316};
        search = {28, 373, 348, 409};
        filter = {360, 373, 520, 409};
        exportButton = {w - 144, 373, w - 28, 409};
        table = {28, 422, w - 28, h - 46};
        tableHeader = {table.left, table.top, table.right, table.top + 40};
        body = {table.left, tableHeader.bottom, table.right - 12, table.bottom};
        track = {table.right - 10, body.top + 4, table.right - 4, table.bottom - 4};
        const float tw = body.right - body.left;
        constexpr float ratios[] = {0, .115f, .475f, .665f, .855f, 1};
        for (int i = 0; i < 6; ++i)
            columns[i] = body.left + ratios[i] * tw;
    }
    int columnAt(float x) const {
        for (int i = 0; i < 5; ++i)
            if (x >= columns[i] && x < columns[i + 1])
                return i;
        return 4;
    }
    int visibleRows() const {
        return std::max(1, static_cast<int>((body.bottom - body.top) / rowHeight));
    }
    Rect thumb(std::size_t count, std::size_t first) const {
        const float h = track.bottom - track.top;
        const float total = static_cast<float>(std::max<std::size_t>(1, count));
        const float th = std::min(h, std::max(28.f, h * visibleRows() / total));
        const auto maxFirst = count > static_cast<std::size_t>(visibleRows()) ? count - visibleRows() : 0;
        const float y =
            track.top + (maxFirst ? (h - th) * static_cast<float>(first) / static_cast<float>(maxFirst) : 0);
        return {track.left, y, track.right, y + th};
    }
};
}
