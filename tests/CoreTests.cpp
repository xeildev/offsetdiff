#include "core/Comparator.hpp"
#include "core/Exporter.hpp"
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

using namespace offsetdiff;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void rejects(std::string_view input) {
    try {
        (void)parseText(input);
    } catch (const ParseError&) {
        return;
    }
    throw std::runtime_error("Malformed/unsupported input was accepted");
}
void identical() {
    const auto a = parseText(R"({"client":{"entity":"0xFF","health":334}})");
    const auto b = parseText(R"({"client":{"health":"0x14e","entity":255}})");
    const auto c = compare(a, b);
    require(c.identical && c.counts[3] == 2, "Semantic equality or exact number normalization failed");
    require(compare(parseText("{}"), parseText("{}")).identical, "Empty objects should be identical");
}
void statuses() {
    const auto c = compare(parseText(R"({"changed":"0xFF","removed":1,"same":0})"),
                           parseText(R"({"changed":"0x100","added":2,"same":"0x0"})"));
    require(c.counts == std::array<std::size_t, 4>{1, 1, 1, 1}, "Wrong status counts");
    require(c.rows[0].name == "added" && !c.rows[0].oldValue, "Added value must have no old value");
    require(c.rows[1].delta == "+0x1", "Changed delta is wrong");
    require(!c.rows[2].newValue, "Removed value must have no new value");
}
void nesting() {
    const auto a = parseText(R"({"a":{"b":{"offset":1}},"other":{"offset":2}})");
    require(a.entries.at("a.b.offset").magnitude == 1, "Nested path lost");
    require(a.entries.at("other.offset").magnitude == 2, "Equal leaf names were mixed up");
}
void collisions() {
    const auto a = parseText(R"({"a.b":1,"a":{"b":2},"":3,"\\e":4,"x\\y":5,"x":{"y":6}})");
    require(a.entries.size() == 6, "Escaped key collision");
    require(a.entries.at("a\\.b").magnitude == 1 && a.entries.at("a.b").magnitude == 2, "Literal dot collision");
    require(a.entries.at("\\e").magnitude == 3 && a.entries.at("\\\\e").magnitude == 4, "Empty key collision");
    rejects(R"({"a":1,"a":2})");
    rejects(R"({"a":{"x":1,"x":2}})");
    rejects(R"({"a":{},"a":{"x":1}})");
    rejects(R"({"a":{},"\u0061":1})");
}
void malformed() {
    for (const auto s : {"", " ", "{", "{\"a\":}", "[]", "1", "null", "{\"a\":true}", "{\"a\":null}", "{\"a\":[]}",
                         "{\"a\":1.5}", "{\"a\":1e3}", "{\"a\":\"123\"}", "{\"a\":\"0x\"}", "{\"a\":\"0xGG\"}",
                         "{\"a\":\"0x-1\"}", "{\"a\":\"0x1 \"}", "{}{}"})
        rejects(s);
    rejects(std::string("{\"a") + '\xFF' + "\":1}");
    std::string deep;
    for (int i = 0; i < 260; ++i)
        deep += "{\"a\":";
    deep += "0";
    deep.append(260, '}');
    rejects(deep);
}
void boundaries() {
    const auto a = parseText(
        R"({"max":"0xFFFFFFFFFFFFFFFF","exact":9007199254740993,"min":-9223372036854775808,"umax":18446744073709551615,"zero":"-0x0"})");
    require(a.entries.at("max").magnitude == UINT64_MAX, "UINT64_MAX hex failed");
    require(a.entries.at("umax").magnitude == UINT64_MAX, "UINT64_MAX integer failed");
    require(a.entries.at("exact").magnitude == 9007199254740993ULL, "Integer lost precision");
    require(a.entries.at("min").hex() == "-0x8000000000000000", "INT64_MIN failed");
    require(!a.entries.at("zero").negative, "Negative zero must normalize");
    require(difference({false, UINT64_MAX}, {false, 0}) == "-0xFFFFFFFFFFFFFFFF", "Large negative delta overflow");
    require(difference({false, 0}, {false, UINT64_MAX}) == "+0xFFFFFFFFFFFFFFFF", "Large positive delta overflow");
    require(difference({true, 1ULL << 63}, {false, UINT64_MAX}) == "+0x17FFFFFFFFFFFFFFF", "65-bit delta overflow");
    require(difference({false, UINT64_MAX}, {true, 1ULL << 63}) == "-0x17FFFFFFFFFFFFFFF",
            "65-bit negative delta overflow");
    require(difference({true, 5}, {true, 3}) == "+0x2", "Negative offset subtraction failed");
    require(difference({true, 3}, {true, 5}) == "-0x2", "Negative offset subtraction sign failed");
    for (const auto s : {R"({"a":18446744073709551616})", R"({"a":-9223372036854775809})",
                         R"({"a":"0x10000000000000000"})", R"({"a":"-0x8000000000000001"})"})
        rejects(s);
}
void unicode() {
    const auto a = parseText(R"({"客户端":{"生命":123},"offset_\u00E4":456})");
    require(a.entries.at("客户端.生命").magnitude == 123, "Unicode key lost");
    require(a.entries.at("offset_\u00E4").magnitude == 456, "Unicode UTF-8 key lost");
    const auto bom = parseText(std::string("\xEF\xBB\xBF") + "{\"a\":1}");
    require(bom.entries.size() == 1, "UTF-8 BOM unsupported");
}
void large() {
    std::string text = "{\"large\":{";
    for (unsigned i = 0; i < 100000; ++i) {
        if (i)
            text += ',';
        text += '"' + std::to_string(i) + "\":" + std::to_string(9007199254740993ULL + i);
    }
    text += "}}";
    const auto a = parseText(text);
    require(a.entries.size() == 100000, "Large dump lost rows");
    const auto c = compare(a, a);
    require(c.rows.size() == 100000 && c.identical, "Large comparison failed");
}
void cancellation() {
    try {
        (void)parseText("{\"x\":1}", [] { return true; });
    } catch (const Cancelled&) {
        return;
    }
    throw std::runtime_error("Parser cancellation failed");
}
void exporting() {
    const auto c =
        compare(parseText(R"({"a":1,"gone":2})"), parseText(R"({"a":18446744073709551615,"new":3,"tab\tname":4})"));
    std::ostringstream out;
    exportStream(out, c, ExportFormat::Json);
    const auto j = nlohmann::json::parse(out.str());
    require(j["rows"].size() == 4 && j["summary"]["changed"] == 1, "JSON export summary failed");
    require(j["rows"][0]["new"] == "0xFFFFFFFFFFFFFFFF", "Export lost precision");
    require(j["rows"][1]["new"].is_null(), "Export must preserve missing values");
    std::ostringstream txt;
    exportStream(txt, c, ExportFormat::Text);
    require(txt.str().find("tab\\tname") != std::string::npos, "TXT control characters not escaped");
    std::ostringstream cancelled;
    bool stopped = false;
    try {
        exportStream(cancelled, c, ExportFormat::Json, [] { return true; });
    } catch (const Cancelled&) {
        stopped = true;
    }
    require(stopped, "Export cancellation failed");
}
void fileIO() {
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("offsetdiff-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    } cleanup{dir};
    const auto input = dir / L"dump-\u00E4-\u751F.json";
    {
        std::ofstream file(input);
        file << "{\"x\":1}";
    }
    const auto a = parseFile(input);
    require(a.entries.size() == 1 && a.source == input && a.bytes == 7, "Unicode file read failed");
    const auto dest = dir / "result.json";
    const auto c = compare(a, a);
    exportFile(dest, c, ExportFormat::Json);
    exportFile(dest, c, ExportFormat::Text);
    std::ifstream read(dest);
    std::string line;
    std::getline(read, line);
    require(line == "xeil / offsetdiff 0.1.0", "Atomic overwrite failed");
    read.close();
    bool cancelled = false;
    try {
        exportFile(dest, c, ExportFormat::Json, [] { return true; });
    } catch (const Cancelled&) {
        cancelled = true;
    }
    require(cancelled, "File export cancellation failed");
    std::ifstream preserved(dest);
    std::getline(preserved, line);
    require(line == "xeil / offsetdiff 0.1.0", "Cancelled export corrupted original");
    require(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 2,
            "Temporary export file leaked");
    bool failed = false;
    try {
        (void)parseFile(dir / "missing.json");
    } catch (const ParseError&) {
        failed = true;
    }
    require(failed, "Missing file accepted");
    const auto empty = dir / "empty.json";
    {
        std::ofstream f(empty);
    }
    failed = false;
    try {
        (void)parseFile(empty);
    } catch (const ParseError&) {
        failed = true;
    }
    require(failed, "Empty file accepted");
}
}
int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {{"identical and normalized dumps", identical},
                                                                   {"changed, added, removed, unchanged", statuses},
                                                                   {"nested objects", nesting},
                                                                   {"duplicate and colliding paths", collisions},
                                                                   {"invalid, empty and unsupported JSON", malformed},
                                                                   {"64-bit boundaries and 65-bit deltas", boundaries},
                                                                   {"Unicode and UTF-8 BOM", unicode},
                                                                   {"100000 offsets", large},
                                                                   {"cancellation", cancellation},
                                                                   {"JSON/TXT export", exporting},
                                                                   {"Unicode files and atomic export", fileIO}};
    int failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS  " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL  " << name << ": " << e.what() << '\n';
        }
    }
    std::cout << (std::size(tests) - failures) << '/' << std::size(tests) << " test groups passed\n";
    return failures ? 1 : 0;
}
