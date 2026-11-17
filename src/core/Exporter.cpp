#include "Exporter.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace offsetdiff {
namespace {
std::string txtEscape(std::string_view input) {
    std::string result;
    for (const char c : input) {
        if (c == '\t')
            result += "\\t";
        else if (c == '\n')
            result += "\\n";
        else if (c == '\r')
            result += "\\r";
        else if (static_cast<unsigned char>(c) < 32) {
            constexpr char hex[] = "0123456789ABCDEF";
            result += "\\x";
            result += hex[(static_cast<unsigned char>(c) >> 4)];
            result += hex[c & 15];
        } else
            result += c;
    }
    return result;
}
}
void exportStream(std::ostream& out, const Comparison& result, ExportFormat format, const Cancel& cancel) {
    using nlohmann::json;
    if (format == ExportFormat::Json) {
        json summary = {{"changed", result.counts[0]},
                        {"added", result.counts[1]},
                        {"removed", result.counts[2]},
                        {"unchanged", result.counts[3]}};
        out << "{\n  \"application\": \"xeil / offsetdiff\",\n  \"version\": \"0.1.0\",\n  \"summary\": "
            << summary.dump() << ",\n  \"identical\": " << (result.identical ? "true" : "false") << ",\n  \"rows\": [";
        bool first = true;
        for (const auto& row : result.rows) {
            if (cancel && cancel())
                throw Cancelled();
            json item = {{"status", statusName(row.status)},
                         {"name", row.name},
                         {"old", row.oldValue ? json(row.oldValue->hex()) : json(nullptr)},
                         {"new", row.newValue ? json(row.newValue->hex()) : json(nullptr)},
                         {"difference", row.delta.empty() ? json(nullptr) : json(row.delta)}};
            out << (first ? "\n    " : ",\n    ") << item.dump();
            first = false;
        }
        out << "\n  ]\n}\n";
    } else {
        out << "xeil / offsetdiff 0.1.0\nChanged: " << result.counts[0] << " | Added: " << result.counts[1]
            << " | Removed: " << result.counts[2] << " | Unchanged: " << result.counts[3]
            << "\nSTATUS\tOFFSET NAME\tOLD VALUE\tNEW VALUE\tDIFFERENCE\n";
        for (const auto& row : result.rows) {
            if (cancel && cancel())
                throw Cancelled();
            out << statusName(row.status) << '\t' << txtEscape(row.name) << '\t'
                << (row.oldValue ? row.oldValue->hex() : "-") << '\t' << (row.newValue ? row.newValue->hex() : "-")
                << '\t' << (row.delta.empty() ? "-" : row.delta) << '\n';
        }
    }
    if (!out)
        throw std::runtime_error("Writing the export failed. Check free disk space and permissions.");
}
void exportFile(const std::filesystem::path& path, const Comparison& result, ExportFormat format,
                const Cancel& cancel) {
    static std::atomic<unsigned> sequence{};
    auto temp = path;
    temp += ".offsetdiff-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence++) + ".tmp";
    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("Cannot create export file. Check the destination folder and permissions.");
        exportStream(out, result, format, cancel);
        out.flush();
        if (!out)
            throw std::runtime_error("Flushing the export failed.");
        out.close();
        if (!out)
            throw std::runtime_error("Closing the export failed.");
        if (cancel && cancel())
            throw Cancelled();
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace the destination file. It may be locked or read-only.");
#else
        std::filesystem::rename(temp, path);
#endif
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        throw;
    }
}
}
