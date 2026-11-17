#include "Comparator.hpp"
#include <algorithm>

namespace offsetdiff {
std::string_view statusName(Status status) {
    constexpr std::string_view names[] = {"CHANGED", "ADDED", "REMOVED", "UNCHANGED"};
    return names[static_cast<unsigned>(status)];
}
std::string difference(const Value& oldValue, const Value& newValue) {
    if (oldValue == newValue)
        return "0x0";
    bool negative{};
    std::uint64_t magnitude{};
    bool carry{};
    if (oldValue.negative != newValue.negative) {
        magnitude = oldValue.magnitude + newValue.magnitude;
        carry = magnitude < oldValue.magnitude;
        negative = newValue.negative;
    } else if (!oldValue.negative) {
        negative = newValue.magnitude < oldValue.magnitude;
        magnitude = negative ? oldValue.magnitude - newValue.magnitude : newValue.magnitude - oldValue.magnitude;
    } else {
        negative = newValue.magnitude > oldValue.magnitude;
        magnitude = negative ? newValue.magnitude - oldValue.magnitude : oldValue.magnitude - newValue.magnitude;
    }
    auto hex = Value{false, magnitude}.hex().substr(2);
    if (carry) {
        hex.insert(0, 16 - hex.size(), '0');
        hex.insert(0, "1");
    }
    return std::string(negative ? "-0x" : "+0x") + hex;
}
Comparison compare(const Dump& oldDump, const Dump& newDump, const Cancel& cancel) {
    Comparison result;
    result.rows.reserve(std::max(oldDump.entries.size(), newDump.entries.size()));
    auto a = oldDump.entries.begin();
    auto b = newDump.entries.begin();
    while (a != oldDump.entries.end() || b != newDump.entries.end()) {
        if (cancel && cancel())
            throw Cancelled();
        Row row;
        if (b == newDump.entries.end() || (a != oldDump.entries.end() && a->first < b->first)) {
            row = {Status::Removed, a->first, a->second, {}, {}};
            ++a;
        } else if (a == oldDump.entries.end() || b->first < a->first) {
            row = {Status::Added, b->first, {}, b->second, {}};
            ++b;
        } else {
            row = {a->second == b->second ? Status::Unchanged : Status::Changed, a->first, a->second, b->second,
                   difference(a->second, b->second)};
            ++a;
            ++b;
        }
        ++result.counts[static_cast<unsigned>(row.status)];
        result.rows.push_back(std::move(row));
    }
    result.identical = result.counts[0] == 0 && result.counts[1] == 0 && result.counts[2] == 0;
    return result;
}
}
