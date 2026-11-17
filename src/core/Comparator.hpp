#pragma once
#include "Parser.hpp"
#include <array>
#include <optional>
#include <vector>

namespace offsetdiff {
enum class Status : unsigned { Changed, Added, Removed, Unchanged };
std::string_view statusName(Status status);
struct Row {
    Status status{};
    std::string name;
    std::optional<Value> oldValue, newValue;
    std::string delta;
};
struct Comparison {
    std::vector<Row> rows;
    std::array<std::size_t, 4> counts{};
    bool identical{};
};
// The delta can require 65 magnitude bits. This returns its exact signed hex form.
std::string difference(const Value& oldValue, const Value& newValue);
Comparison compare(const Dump& oldDump, const Dump& newDump, const Cancel& cancel = {});
}
