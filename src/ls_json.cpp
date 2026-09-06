// ls_json.cpp — dump and parse for the serializer JSON value.

#include "ls_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ls {
namespace json {
namespace {

void dumpString(const std::string& text, std::string& out) {
    out.push_back('"');
    for (char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c) & 0xffu);
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void dumpNumber(double value, std::string& out) {
    if (!std::isfinite(value)) {
        out += "0";
        return;
    }
    // Integers print without a decimal point; everything else uses the shortest
    // representation that round-trips exactly.
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        out += buffer;
        return;
    }
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    out += buffer;
}

void dumpValue(const Value& value, std::string& out) {
    switch (value.type()) {
        case Value::Type::Null:   out += "null"; break;
        case Value::Type::Bool:   out += value.asBool() ? "true" : "false"; break;
        case Value::Type::Number: dumpNumber(value.asNumber(), out); break;
        case Value::Type::String: dumpString(value.asString(), out); break;
        case Value::Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const Value& item : value.items()) {
                if (!first) { out.push_back(','); }
                first = false;
                dumpValue(item, out);
            }
            out.push_back(']');
            break;
        }
        case Value::Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, member] : value.members()) {
                if (!first) { out.push_back(','); }
                first = false;
                dumpString(key, out);
                out.push_back(':');
                dumpValue(member, out);
            }
            out.push_back('}');
            break;
        }
    }
}

struct Parser {
    const std::string& text;
    size_t pos = 0;

    void skipWhitespace() {
        while (pos < text.size() &&
               (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
            ++pos;
        }
    }

    bool parseValue(Value& out) {
        skipWhitespace();
        if (pos >= text.size()) {
            return false;
        }
        const char c = text[pos];
        switch (c) {
            case 'n': return literal("null", out, Value{});
            case 't': return literal("true", out, Value{true});
            case 'f': return literal("false", out, Value{false});
            case '"': return parseString(out);
            case '[': return parseArray(out);
            case '{': return parseObject(out);
            default:  return parseNumber(out);
        }
    }

    bool literal(const char* word, Value& out, Value result) {
        const size_t length = std::string(word).size();
        if (text.compare(pos, length, word) != 0) {
            return false;
        }
        pos += length;
        out = std::move(result);
        return true;
    }

    bool parseString(Value& out) {
        if (text[pos] != '"') {
            return false;
        }
        ++pos;
        std::string result;
        while (pos < text.size() && text[pos] != '"') {
            char c = text[pos++];
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (pos >= text.size()) {
                return false;
            }
            const char escape = text[pos++];
            switch (escape) {
                case '"':  result.push_back('"');  break;
                case '\\': result.push_back('\\'); break;
                case '/':  result.push_back('/');  break;
                case 'n':  result.push_back('\n'); break;
                case 'r':  result.push_back('\r'); break;
                case 't':  result.push_back('\t'); break;
                case 'b':  result.push_back('\b'); break;
                case 'f':  result.push_back('\f'); break;
                case 'u': {
                    if (pos + 4 > text.size()) {
                        return false;
                    }
                    const std::string digits = text.substr(pos, 4);
                    pos += 4;
                    const long code = std::strtol(digits.c_str(), nullptr, 16);
                    if (code < 0x80) {
                        result.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        result.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        result.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        }
        if (pos >= text.size()) {
            return false;
        }
        ++pos;   // closing quote
        out = Value{std::move(result)};
        return true;
    }

    bool parseNumber(Value& out) {
        const size_t start = pos;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
            ++pos;
        }
        bool digits = false;
        while (pos < text.size() &&
               ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == '.' ||
                text[pos] == 'e' || text[pos] == 'E' || text[pos] == '-' || text[pos] == '+')) {
            digits = digits || (text[pos] >= '0' && text[pos] <= '9');
            ++pos;
        }
        if (!digits) {
            return false;
        }
        out = Value{std::strtod(text.substr(start, pos - start).c_str(), nullptr)};
        return true;
    }

    bool parseArray(Value& out) {
        ++pos;   // [
        Value result = Value::array();
        skipWhitespace();
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            out = std::move(result);
            return true;
        }
        while (pos < text.size()) {
            Value item;
            if (!parseValue(item)) {
                return false;
            }
            result.push(std::move(item));
            skipWhitespace();
            if (pos >= text.size()) {
                return false;
            }
            if (text[pos] == ',') {
                ++pos;
                continue;
            }
            if (text[pos] == ']') {
                ++pos;
                out = std::move(result);
                return true;
            }
            return false;
        }
        return false;
    }

    bool parseObject(Value& out) {
        ++pos;   // {
        Value result = Value::object();
        skipWhitespace();
        if (pos < text.size() && text[pos] == '}') {
            ++pos;
            out = std::move(result);
            return true;
        }
        while (pos < text.size()) {
            skipWhitespace();
            Value key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (pos >= text.size() || text[pos] != ':') {
                return false;
            }
            ++pos;
            Value member;
            if (!parseValue(member)) {
                return false;
            }
            result[key.asString()] = std::move(member);
            skipWhitespace();
            if (pos >= text.size()) {
                return false;
            }
            if (text[pos] == ',') {
                ++pos;
                continue;
            }
            if (text[pos] == '}') {
                ++pos;
                out = std::move(result);
                return true;
            }
            return false;
        }
        return false;
    }
};

} // namespace

std::string dump(const Value& value) {
    std::string out;
    dumpValue(value, out);
    return out;
}

bool parse(const std::string& text, Value& out) {
    Parser parser{text};
    if (!parser.parseValue(out)) {
        return false;
    }
    parser.skipWhitespace();
    return parser.pos == text.size();
}

} // namespace json
} // namespace ls
