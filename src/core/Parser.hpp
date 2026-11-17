#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace offsetdiff {
struct Value {
    bool negative{};
    std::uint64_t magnitude{};
    bool operator==(const Value&) const = default;
    std::string hex() const;
};
using Entries = std::map<std::string, Value, std::less<>>;
struct Dump {
    Entries entries;
    std::filesystem::path source;
    std::uintmax_t bytes{};
};
using Cancel = std::function<bool()>;
class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("Operation cancelled") {}
};
class ParseError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
inline constexpr std::uintmax_t MaxFileBytes = 512ULL * 1024 * 1024;
// Backslashes and dots are escaped; an empty key is represented as \e.
std::string escapeKey(std::string_view key);
Dump parseText(std::string_view text, const Cancel& cancel = {});
Dump parseFile(const std::filesystem::path& path, const Cancel& cancel = {});
}
