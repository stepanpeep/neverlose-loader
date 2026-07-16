#include "MiniJson.h"
#include <cctype>
#include <cstdlib>
#include <stdexcept>

bool JsonValue::isObject() const { return std::holds_alternative<Object>(value_); }
bool JsonValue::isArray() const { return std::holds_alternative<Array>(value_); }
const JsonValue::Object& JsonValue::object() const { static Object empty; auto p = std::get_if<Object>(&value_); return p ? *p : empty; }
const JsonValue::Array& JsonValue::array() const { static Array empty; auto p = std::get_if<Array>(&value_); return p ? *p : empty; }
std::string JsonValue::string(std::string fallback) const { auto p = std::get_if<std::string>(&value_); return p ? *p : std::move(fallback); }
bool JsonValue::boolean(bool fallback) const { auto p = std::get_if<bool>(&value_); return p ? *p : fallback; }
double JsonValue::number(double fallback) const { auto p = std::get_if<double>(&value_); return p ? *p : fallback; }
const JsonValue& JsonValue::get(const std::string& key) const { static JsonValue empty; auto p = std::get_if<Object>(&value_); if (!p) return empty; auto it = p->find(key); return it == p->end() ? empty : it->second; }

namespace {
void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp <= 0xFFFF) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
}

class Parser {
public:
    explicit Parser(const std::string& input) : text_(input) {}
    JsonValue run() { skip(); auto value = parseValue(); skip(); if (pos_ != text_.size()) fail("unexpected trailing data"); return value; }
private:
    const std::string& text_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const char* message) const { throw std::runtime_error(std::string(message) + " at byte " + std::to_string(pos_)); }
    void skip() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    bool take(char c) { skip(); if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; } return false; }
    void literal(const char* word) { while (*word) if (pos_ >= text_.size() || text_[pos_++] != *word++) fail("invalid literal"); }
    unsigned hex4() { unsigned value = 0; for (int i = 0; i < 4; ++i) { if (pos_ >= text_.size()) fail("incomplete unicode escape"); char c = text_[pos_++]; value <<= 4; if (c >= '0' && c <= '9') value |= c - '0'; else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10; else fail("invalid unicode escape"); } return value; }

    JsonValue parseValue() {
        skip(); if (pos_ >= text_.size()) fail("unexpected end");
        switch (text_[pos_]) {
            case '{': return parseObject(); case '[': return parseArray(); case '"': return JsonValue(parseString());
            case 't': literal("true"); return JsonValue(true); case 'f': literal("false"); return JsonValue(false); case 'n': literal("null"); return JsonValue();
            default: if (text_[pos_] == '-' || std::isdigit(static_cast<unsigned char>(text_[pos_]))) return JsonValue(parseNumber()); fail("invalid value");
        }
    }

    JsonValue parseObject() {
        take('{'); JsonValue::Object object; if (take('}')) return JsonValue(object);
        do { skip(); if (pos_ >= text_.size() || text_[pos_] != '"') fail("object key expected"); auto key = parseString(); if (!take(':')) fail("colon expected"); object.insert_or_assign(std::move(key), parseValue()); } while (take(','));
        if (!take('}')) fail("closing brace expected");
        return JsonValue(object);
    }

    JsonValue parseArray() {
        take('['); JsonValue::Array array; if (take(']')) return JsonValue(array);
        do { array.push_back(parseValue()); } while (take(','));
        if (!take(']')) fail("closing bracket expected");
        return JsonValue(array);
    }

    std::string parseString() {
        if (text_[pos_++] != '"') fail("quote expected");
        std::string out;
        while (pos_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return out;
            if (c < 0x20) fail("control character in string");
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (pos_ >= text_.size()) fail("bad escape");
            char e = text_[pos_++];
            switch (e) {
                case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': { unsigned cp = hex4(); if (cp >= 0xD800 && cp <= 0xDBFF) { if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') fail("missing low surrogate"); pos_ += 2; unsigned low = hex4(); if (low < 0xDC00 || low > 0xDFFF) fail("invalid low surrogate"); cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00); } appendUtf8(out, cp); break; }
                default: fail("invalid escape");
            }
        }
        fail("unterminated string");
    }

    double parseNumber() { const char* begin = text_.c_str() + pos_; char* end = nullptr; double value = std::strtod(begin, &end); if (end == begin) fail("invalid number"); pos_ = static_cast<size_t>(end - text_.c_str()); return value; }
};
}

bool MiniJson::parse(const std::string& text, JsonValue& output, std::string& error) {
    try { output = Parser(text).run(); error.clear(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
