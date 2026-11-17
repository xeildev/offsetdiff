#pragma once
#include <d2d1.h>

namespace offsetdiff::ui {
constexpr D2D1_COLOR_F rgb(unsigned value, float alpha = 1.f) {
    return {((value >> 16) & 255) / 255.f, ((value >> 8) & 255) / 255.f, (value & 255) / 255.f, alpha};
}
namespace theme {
inline constexpr auto background = rgb(0x0D0D0F);
inline constexpr auto panel = rgb(0x17171B);
inline constexpr auto border = rgb(0x29292E);
inline constexpr auto text = rgb(0xF5F5F5);
inline constexpr auto muted = rgb(0x85858D);
inline constexpr auto hover = rgb(0x222227);
inline constexpr auto selected = rgb(0x2A2A31);
inline constexpr auto red = rgb(0xD2A5A5);
}
}
