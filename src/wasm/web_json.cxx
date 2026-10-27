#include "web_json.hxx"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "table_types.hxx"

namespace
{
    class Parser
    {
    public:
        explicit Parser(std::string_view text) : text_(text) {}

        JsonValue document()
        {
            JsonValue value = parseValue(0);
            skipSpace();
            if (pos_ != text_.size())
                fail("trailing characters");
            return value;
        }

    private:
        static constexpr int MAX_DEPTH = 64;

        std::string_view text_;
        size_t pos_ = 0;

        [[noreturn]] void fail(const char *what) const
        {
            throw std::invalid_argument(std::string("JSON: ") + what + " at " + std::to_string(pos_));
        }

        void skipSpace()
        {
            while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
                ++pos_;
        }

        bool consume(std::string_view word)
        {
            if (text_.substr(pos_, word.size()) != word)
                return false;
            pos_ += word.size();
            return true;
        }

        JsonValue parseValue(int depth)
        {
            if (depth > MAX_DEPTH)
                fail("nested too deeply");
            skipSpace();
            if (pos_ >= text_.size())
                fail("unexpected end");
            const char c = text_[pos_];
            if (c == '{')
                return parseObject(depth);
            if (c == '[')
                return parseArray(depth);
            if (c == '"')
                return JsonValue(parseString());
            if (consume("true"))
                return JsonValue(true);
            if (consume("false"))
                return JsonValue(false);
            if (consume("null"))
                return JsonValue();
            return JsonValue(parseNumber());
        }

        JsonValue parseObject(int depth)
        {
            ++pos_; // {
            JsonValue::Object object;
            skipSpace();
            if (consume("}"))
                return JsonValue(std::move(object));
            for (;;)
            {
                skipSpace();
                if (pos_ >= text_.size() || text_[pos_] != '"')
                    fail("expected a key");
                std::string name = parseString();
                skipSpace();
                if (!consume(":"))
                    fail("expected ':'");
                object.insert_or_assign(std::move(name), parseValue(depth + 1));
                skipSpace();
                if (consume("}"))
                    return JsonValue(std::move(object));
                if (!consume(","))
                    fail("expected ',' or '}'");
            }
        }

        JsonValue parseArray(int depth)
        {
            ++pos_; // [
            JsonValue::Array array;
            skipSpace();
            if (consume("]"))
                return JsonValue(std::move(array));
            for (;;)
            {
                array.push_back(parseValue(depth + 1));
                skipSpace();
                if (consume("]"))
                    return JsonValue(std::move(array));
                if (!consume(","))
                    fail("expected ',' or ']'");
            }
        }

        double parseNumber()
        {
            const size_t start = pos_;
            while (pos_ < text_.size() && std::string_view("+-0123456789.eE").find(text_[pos_]) != std::string_view::npos)
                ++pos_;
            const auto value = parseDouble(text_.substr(start, pos_ - start));
            if (!value)
                fail("invalid value");
            return *value;
        }

        unsigned hex4()
        {
            if (pos_ + 4 > text_.size())
                fail("short \\u escape");
            unsigned code = 0;
            const auto result = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, code, 16);
            if (result.ec != std::errc() || result.ptr != text_.data() + pos_ + 4)
                fail("invalid \\u escape");
            pos_ += 4;
            return code;
        }

        static void appendUtf8(std::string &out, unsigned code)
        {
            if (code < 0x80)
                out += static_cast<char>(code);
            else if (code < 0x800)
            {
                out += static_cast<char>(0xc0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3f));
            }
            else if (code < 0x10000)
            {
                out += static_cast<char>(0xe0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (code & 0x3f));
            }
            else
            {
                out += static_cast<char>(0xf0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (code & 0x3f));
            }
        }

        std::string parseString()
        {
            ++pos_; // "
            std::string out;
            for (;;)
            {
                if (pos_ >= text_.size())
                    fail("unterminated string");
                const char c = text_[pos_++];
                if (c == '"')
                    return out;
                if (static_cast<unsigned char>(c) < 0x20)
                    fail("control character in a string");
                if (c != '\\')
                {
                    out += c;
                    continue;
                }
                if (pos_ >= text_.size())
                    fail("unterminated string");
                switch (text_[pos_++])
                {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                {
                    unsigned code = hex4();
                    // A surrogate pair; a lone surrogate becomes U+FFFD.
                    if (code >= 0xd800 && code < 0xdc00 && consume("\\u"))
                    {
                        const unsigned low = hex4();
                        code = low >= 0xdc00 && low < 0xe000 ? 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00) : 0xfffd;
                    }
                    else if (code >= 0xd800 && code < 0xe000)
                        code = 0xfffd;
                    appendUtf8(out, code);
                    break;
                }
                default:
                    fail("invalid escape");
                }
            }
        }
    };

    const JsonValue NULL_VALUE;
}

std::optional<bool> JsonValue::boolean() const
{
    if (const auto *value = std::get_if<bool>(&value_))
        return *value;
    return std::nullopt;
}

std::optional<double> JsonValue::number() const
{
    if (const auto *value = std::get_if<double>(&value_))
        return *value;
    return std::nullopt;
}

const std::string *JsonValue::string() const
{
    return std::get_if<std::string>(&value_);
}

const JsonValue::Array *JsonValue::array() const
{
    const auto *value = std::get_if<std::shared_ptr<Array>>(&value_);
    return value ? value->get() : nullptr;
}

const JsonValue::Object *JsonValue::object() const
{
    const auto *value = std::get_if<std::shared_ptr<Object>>(&value_);
    return value ? value->get() : nullptr;
}

const JsonValue &JsonValue::operator[](std::string_view key) const
{
    if (const Object *members = object())
    {
        if (const auto it = members->find(key); it != members->end())
            return it->second;
    }
    return NULL_VALUE;
}

JsonValue JsonValue::parse(std::string_view text)
{
    return Parser(text).document();
}

void JsonWriter::separate()
{
    if (after_key_)
    {
        after_key_ = false;
        return;
    }
    if (!first_.empty())
    {
        if (!first_.back())
            text_ += ',';
        first_.back() = false;
    }
}

JsonWriter &JsonWriter::beginObject()
{
    separate();
    text_ += '{';
    first_.push_back(true);
    return *this;
}

JsonWriter &JsonWriter::endObject()
{
    text_ += '}';
    first_.pop_back();
    return *this;
}

JsonWriter &JsonWriter::beginArray()
{
    separate();
    text_ += '[';
    first_.push_back(true);
    return *this;
}

JsonWriter &JsonWriter::endArray()
{
    text_ += ']';
    first_.pop_back();
    return *this;
}

JsonWriter &JsonWriter::key(std::string_view name)
{
    value(name);
    text_ += ':';
    after_key_ = true;
    return *this;
}

JsonWriter &JsonWriter::value(std::string_view text)
{
    separate();
    text_ += '"';
    for (const char c : text)
    {
        switch (c)
        {
        case '"': text_ += "\\\""; break;
        case '\\': text_ += "\\\\"; break;
        case '\n': text_ += "\\n"; break;
        case '\r': text_ += "\\r"; break;
        case '\t': text_ += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                text_ += escaped;
            }
            else
                text_ += c;
        }
    }
    text_ += '"';
    return *this;
}

JsonWriter &JsonWriter::value(bool value)
{
    separate();
    text_ += value ? "true" : "false";
    return *this;
}

JsonWriter &JsonWriter::value(double value)
{
    separate();
    if (!std::isfinite(value))
    {
        text_ += "null";
        return *this;
    }
    text_ += shortestDoubleText(value);
    return *this;
}

JsonWriter &JsonWriter::value(size_t value)
{
    separate();
    text_ += std::to_string(value);
    return *this;
}

JsonWriter &JsonWriter::value(int value)
{
    separate();
    text_ += std::to_string(value);
    return *this;
}

JsonWriter &JsonWriter::null()
{
    separate();
    text_ += "null";
    return *this;
}

JsonWriter &JsonWriter::raw(std::string_view json)
{
    separate();
    text_ += json;
    return *this;
}
