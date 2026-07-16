#pragma once
#include <map>
#include <string>
#include <variant>
#include <vector>

class JsonValue {
public:
    using Object = std::map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    JsonValue() : value_(nullptr) {}
    explicit JsonValue(Storage value) : value_(std::move(value)) {}

    bool isObject() const;
    bool isArray() const;
    const Object& object() const;
    const Array& array() const;
    std::string string(std::string fallback = {}) const;
    bool boolean(bool fallback = false) const;
    double number(double fallback = 0.0) const;
    const JsonValue& get(const std::string& key) const;

private:
    Storage value_;
};

class MiniJson {
public:
    static bool parse(const std::string& text, JsonValue& output, std::string& error);
};
