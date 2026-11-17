#pragma once
#include "Comparator.hpp"
#include <iosfwd>

namespace offsetdiff {
enum class ExportFormat { Json, Text };
void exportStream(std::ostream& out, const Comparison& result, ExportFormat format, const Cancel& cancel = {});
// Writes a sibling temporary file, then replaces the destination only after success.
void exportFile(const std::filesystem::path& path, const Comparison& result, ExportFormat format,
                const Cancel& cancel = {});
}
