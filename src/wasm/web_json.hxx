#ifndef _WEB_JSON_HXX_
#define _WEB_JSON_HXX_

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// A small JSON reader and writer for the messages between the web frontend
// and WebSession: requests are small, replies are built as text.

class JsonValue
{
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;

    JsonValue() = default;
    JsonValue(bool value) : value_(value) {}
    JsonValue(double value) : value_(value) {}
    JsonValue(std::string value) : value_(std::move(value)) {}
    JsonValue(const char *value) : value_(std::string(value)) {}
    JsonValue(Array value) : value_(std::make_shared<Array>(std::move(value))) {}
    JsonValue(Object value) : value_(std::make_shared<Object>(std::move(value))) {}

    bool isNull() const { return std::holds_alternative<std::monostate>(value_); }
    // nullptr / nullopt if the value is of another type.
    std::optional<bool> boolean() const;
    std::optional<double> number() const;
    const std::string *string() const;
    const Array *array() const;
    const Object *object() const;

    // A member of an object; a null value if this is not an object or has
    // no such member.
    const JsonValue &operator[](std::string_view key) const;

    // Throws std::invalid_argument on malformed text.
    static JsonValue parse(std::string_view text);

private:
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> value_;
};

// Appends JSON text. Values go after key() inside an object, or straight
// into an array; commas are added as needed. Strings are escaped as JSON
// requires (invalid UTF-8 is passed through: the reader replaces it).
class JsonWriter
{
public:
    JsonWriter &beginObject();
    JsonWriter &endObject();
    JsonWriter &beginArray();
    JsonWriter &endArray();
    JsonWriter &key(std::string_view name);
    JsonWriter &value(std::string_view text);
    JsonWriter &value(const char *text) { return value(std::string_view(text)); }
    JsonWriter &value(bool value);
    JsonWriter &value(double value);
    JsonWriter &value(size_t value);
    JsonWriter &value(int value);
    JsonWriter &null();
    // Appends JSON text as it is, as one value.
    JsonWriter &raw(std::string_view json);

    const std::string &text() const { return text_; }
    std::string take() { return std::move(text_); }

private:
    std::string text_;
    std::vector<bool> first_; // per open container: nothing written yet
    bool after_key_ = false;

    void separate();
};

#endif // _WEB_JSON_HXX_
