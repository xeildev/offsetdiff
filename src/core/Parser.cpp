#include "Parser.hpp"
#include <charconv>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <vector>

namespace offsetdiff {
std::string Value::hex() const {
    char buffer[17]{};
    const auto result = std::to_chars(buffer, buffer + 16, magnitude, 16);
    std::string s(buffer, result.ptr);
    for (auto& c : s)
        if (c >= 'a' && c <= 'f')
            c -= 'a' - 'A';
    return (negative && magnitude ? "-0x" : "0x") + s;
}
std::string escapeKey(std::string_view key) {
    if (key.empty())
        return "\\e";
    std::string result;
    for (const char c : key) {
        if (c == '.' || c == '\\')
            result += '\\';
        result += c;
    }
    return result;
}
namespace {
using Json = nlohmann::json;
struct Sax final : nlohmann::json_sax<Json> {
    struct Frame {
        std::string path;
        std::string key;
        std::unordered_set<std::string> keys;
    };
    Dump dump;
    std::vector<Frame> stack;
    std::string error;
    const Cancel& cancel;
    bool rootSeen{};
    explicit Sax(const Cancel& c) : cancel(c) {}
    bool fail(std::string message) {
        error = std::move(message);
        return false;
    }
    void check() const {
        if (cancel && cancel())
            throw Cancelled();
    }
    std::string path() const {
        if (stack.empty())
            return {};
        const auto& f = stack.back();
        return f.path.empty() ? escapeKey(f.key) : f.path + "." + escapeKey(f.key);
    }
    bool add(Value value) {
        check();
        if (stack.empty())
            return fail("The JSON root must be an object.");
        value.negative = value.negative && value.magnitude != 0;
        const auto p = path();
        if (!dump.entries.emplace(p, value).second)
            return fail("Colliding key path: " + p);
        return true;
    }
    bool null() override {
        return fail("Null is not an offset at: " + path());
    }
    bool boolean(bool) override {
        return fail("Boolean is not an offset at: " + path());
    }
    bool number_integer(number_integer_t n) override {
        const auto magnitude = n < 0 ? std::uint64_t(-(n + 1)) + 1 : std::uint64_t(n);
        return add({n < 0, magnitude});
    }
    bool number_unsigned(number_unsigned_t n) override {
        return add({false, n});
    }
    bool number_float(number_float_t, const string_t&) override {
        return fail("Expected an exact 64-bit integer (no fractions, exponents or overflow) at: " + path());
    }
    bool string(string_t& s) override {
        std::string_view v = s;
        bool negative = false;
        if (!v.empty() && v.front() == '-') {
            negative = true;
            v.remove_prefix(1);
        }
        if (v.size() < 3 || v[0] != '0' || (v[1] != 'x' && v[1] != 'X'))
            return fail("Expected a hexadecimal string beginning with 0x at: " + path());
        v.remove_prefix(2);
        std::uint64_t n{};
        const auto r = std::from_chars(v.data(), v.data() + v.size(), n, 16);
        if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || (negative && n > (std::uint64_t{1} << 63)))
            return fail("Invalid or out-of-range 64-bit hexadecimal value at: " + path());
        return add({negative, n});
    }
    bool binary(binary_t&) override {
        return fail("Binary JSON values are unsupported.");
    }
    bool start_object(std::size_t) override {
        check();
        if (stack.size() >= 256)
            return fail("Maximum object nesting depth (256) exceeded.");
        if (stack.empty()) {
            if (rootSeen)
                return fail("Multiple JSON roots are unsupported.");
            rootSeen = true;
            stack.push_back({});
        } else {
            auto p = path();
            stack.push_back({std::move(p), {}, {}});
        }
        return true;
    }
    bool key(string_t& s) override {
        check();
        auto& f = stack.back();
        if (!f.keys.insert(s).second)
            return fail("Duplicate JSON key in object: " + (f.path.empty() ? "<root>" : f.path) + " / " + s);
        f.key = s;
        return true;
    }
    bool end_object() override {
        stack.pop_back();
        return true;
    }
    bool start_array(std::size_t) override {
        return fail("Arrays are unsupported; expected nested objects at: " + path());
    }
    bool end_array() override {
        return false;
    }
    bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception& e) override {
        return fail("Invalid JSON at byte " + std::to_string(position) + ": " + e.what());
    }
};
}
Dump parseText(std::string_view text, const Cancel& cancel) {
    if (text.size() > MaxFileBytes)
        throw ParseError("File exceeds the 512 MiB safety limit.");
    if (text.empty())
        throw ParseError("The file is empty. Expected a JSON object.");
    Sax sax(cancel);
    if (!Json::sax_parse(text.begin(), text.end(), &sax))
        throw ParseError(sax.error.empty() ? "Invalid JSON offset dump." : sax.error);
    sax.dump.bytes = text.size();
    return std::move(sax.dump);
}
Dump parseFile(const std::filesystem::path& path, const Cancel& cancel) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw ParseError("Cannot open file. Check its location and read permissions.");
    std::string text;
    char chunk[64 * 1024];
    while (file) {
        if (cancel && cancel())
            throw Cancelled();
        file.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (text.size() + count > MaxFileBytes)
            throw ParseError("File exceeds the 512 MiB safety limit.");
        text.append(chunk, count);
    }
    if (file.bad())
        throw ParseError("Reading the file failed.");
    auto dump = parseText(text, cancel);
    dump.source = path;
    return dump;
}
}
