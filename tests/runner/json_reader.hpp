#pragma once

// Minimal JSON reader for `case.json`, golden metadata and timing baselines.
//
// Flattens a document into dotted paths, so `timings_ms.total` and `outputs.0.name` are both plain
// lookups. That is all the runner needs, and it keeps the test tooling free of a JSON dependency
// the spec does not pin.

#include <engine/common.hpp>
#include <engine/error.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace test {

class Json {
public:
    /// Parses `path`. A missing file is an error; malformed input yields whatever was read so far
    /// plus an error, so the caller can report the file name.
    [[nodiscard]] static engine::Result<Json> Load(std::string_view path) {
        std::array<char, 512> nullTerminated{};
        engine::detail::CopyBounded(nullTerminated, path);
        std::FILE* file = std::fopen(nullTerminated.data(), "rb");
        if (file == nullptr) {
            return std::unexpected(engine::MakeError(engine::ErrorCode::NotFound,
                                                     engine::ErrorStage::Export,
                                                     "could not open {}", path));
        }
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        std::string text(static_cast<engine::usize_t>(size > 0 ? size : 0), '\0');
        if (size > 0) {
            const engine::usize_t read = std::fread(text.data(), 1, text.size(), file);
            text.resize(read);
        }
        std::fclose(file);

        Json           json;
        engine::usize_t cursor = 0;
        if (!json.ParseValue(text, cursor, "")) {
            return std::unexpected(engine::MakeError(engine::ErrorCode::IoError,
                                                     engine::ErrorStage::Export,
                                                     "{} is not valid JSON", path));
        }
        return json;
    }

    [[nodiscard]] bool Has(std::string_view key) const { return Find(key) != nullptr; }

    [[nodiscard]] std::string_view Text(std::string_view key,
                                        std::string_view fallback = {}) const {
        const std::string* value = Find(key);
        return value != nullptr ? std::string_view{*value} : fallback;
    }

    [[nodiscard]] engine::f64_t Number(std::string_view key, engine::f64_t fallback) const {
        const std::string* value = Find(key);
        if (value == nullptr || value->empty()) {
            return fallback;
        }
        char*                end    = nullptr;
        const engine::f64_t  parsed = std::strtod(value->c_str(), &end);
        return end == value->c_str() ? fallback : parsed;
    }

    /// Number of elements in the array at `key`, found by counting `key.<n>` prefixes.
    [[nodiscard]] engine::usize_t ArraySize(std::string_view key) const {
        engine::usize_t count = 0;
        for (;; ++count) {
            std::array<char, 128> probe{};
            std::snprintf(probe.data(), probe.size(), "%.*s.%zu", static_cast<int>(key.size()),
                          key.data(), count);
            bool anyChild = false;
            for (const Entry& entry : m_entries) {
                if (entry.key.compare(0, std::strlen(probe.data()), probe.data()) == 0) {
                    anyChild = true;
                    break;
                }
            }
            if (!anyChild) {
                return count;
            }
        }
    }

private:
    struct Entry {
        std::string key;
        std::string value;
    };

    [[nodiscard]] const std::string* Find(std::string_view key) const {
        for (const Entry& entry : m_entries) {
            if (entry.key == key) {
                return &entry.value;
            }
        }
        return nullptr;
    }

    static void SkipSpace(std::string_view text, engine::usize_t& cursor) {
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'
                                        || text[cursor] == '\n' || text[cursor] == '\r')) {
            ++cursor;
        }
    }

    static bool ParseString(std::string_view text, engine::usize_t& cursor, std::string& out) {
        if (cursor >= text.size() || text[cursor] != '"') {
            return false;
        }
        ++cursor;
        out.clear();
        while (cursor < text.size() && text[cursor] != '"') {
            if (text[cursor] == '\\' && cursor + 1 < text.size()) {
                ++cursor;
                switch (text[cursor]) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(text[cursor]); break;
                }
            } else {
                out.push_back(text[cursor]);
            }
            ++cursor;
        }
        if (cursor >= text.size()) {
            return false;
        }
        ++cursor;
        return true;
    }

    /// Records `prefix` -> scalar, or recurses into an object or array extending the prefix.
    bool ParseValue(std::string_view text, engine::usize_t& cursor, const std::string& prefix) {
        SkipSpace(text, cursor);
        if (cursor >= text.size()) {
            return false;
        }

        if (text[cursor] == '{' || text[cursor] == '[') {
            const bool isArray = text[cursor] == '[';
            const char closing = isArray ? ']' : '}';
            ++cursor;
            engine::usize_t index = 0;
            for (;;) {
                SkipSpace(text, cursor);
                if (cursor >= text.size()) {
                    return false;
                }
                if (text[cursor] == closing) {
                    ++cursor;
                    return true;
                }

                std::string childKey;
                if (isArray) {
                    childKey = std::to_string(index++);
                } else {
                    if (!ParseString(text, cursor, childKey)) {
                        return false;
                    }
                    SkipSpace(text, cursor);
                    if (cursor >= text.size() || text[cursor] != ':') {
                        return false;
                    }
                    ++cursor;
                }

                const std::string childPrefix =
                    prefix.empty() ? childKey : prefix + "." + childKey;
                if (!ParseValue(text, cursor, childPrefix)) {
                    return false;
                }

                SkipSpace(text, cursor);
                if (cursor < text.size() && text[cursor] == ',') {
                    ++cursor;
                }
            }
        }

        std::string value;
        if (text[cursor] == '"') {
            if (!ParseString(text, cursor, value)) {
                return false;
            }
        } else {
            const engine::usize_t start = cursor;
            while (cursor < text.size() && text[cursor] != ',' && text[cursor] != '}'
                   && text[cursor] != ']' && text[cursor] != '\n') {
                ++cursor;
            }
            value.assign(text.substr(start, cursor - start));
            while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) {
                value.pop_back();
            }
        }
        m_entries.push_back(Entry{.key = prefix, .value = value});
        return true;
    }

    std::vector<Entry> m_entries;
};

} // namespace test
