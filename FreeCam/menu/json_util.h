// json_util.h - tiny JSON reader/writer used to edit state.json.
//
// The local server (LocalServer/state.py) keeps the profile in state.json and
// only re-reads it when its own .py files change, so the menu writes the file
// and touches LocalServer/handlers.py to make the running server pick it up.
//
// Only the operations needed here are supported: parse to a tree, index into
// objects/arrays, set numbers/strings, and serialise back.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tw {
namespace json {

struct Value {
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    long long integer = 0;
    double number = 0.0;
    std::string str;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Int || type == Type::Double; }

    // Object member access. Creates the member if missing (for building).
    Value& operator[](const std::string& key) {
        for (auto& kv : object)
            if (kv.first == key) return kv.second;
        object.emplace_back(key, Value{});
        return object.back().second;
    }
    const Value* find(const std::string& key) const {
        for (const auto& kv : object)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    Value* find(const std::string& key) {
        for (auto& kv : object)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    Value& at(std::size_t i) { return array.at(i); }
    const Value& at(std::size_t i) const { return array.at(i); }
    std::size_t size() const { return type == Type::Array ? array.size() : object.size(); }

    void setInt(long long v) { type = Type::Int; integer = v; }
    void setDouble(double v) { type = Type::Double; number = v; }
    void setString(std::string v) { type = Type::String; str = std::move(v); }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    Value parse() {
        skipWs();
        Value v = parseValue();
        skipWs();
        if (i_ != s_.size()) throw std::runtime_error("trailing characters in JSON");
        return v;
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;

    void skipWs() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    Value parseValue() {
        skipWs();
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { Value v; v.type = Value::Type::String; v.str = parseString(); return v; }
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { expect("null"); Value v; v.type = Value::Type::Null; return v; }
        return parseNumber();
    }

    void expect(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (i_ >= s_.size() || s_[i_] != *p)
                throw std::runtime_error("unexpected token in JSON");
            ++i_;
        }
    }

    Value parseObject() {
        Value v;
        v.type = Value::Type::Object;
        expect("{");
        skipWs();
        if (peek() == '}') { ++i_; return v; }
        while (true) {
            skipWs();
            std::string key = parseString();
            skipWs();
            if (peek() != ':') throw std::runtime_error("expected ':' in JSON");
            ++i_;
            Value val = parseValue();
            v.object.emplace_back(std::move(key), std::move(val));
            skipWs();
            char c = peek();
            if (c == ',') { ++i_; continue; }
            if (c == '}') { ++i_; break; }
            throw std::runtime_error("expected ',' or '}' in JSON");
        }
        return v;
    }

    Value parseArray() {
        Value v;
        v.type = Value::Type::Array;
        expect("[");
        skipWs();
        if (peek() == ']') { ++i_; return v; }
        while (true) {
            v.array.push_back(parseValue());
            skipWs();
            char c = peek();
            if (c == ',') { ++i_; continue; }
            if (c == ']') { ++i_; break; }
            throw std::runtime_error("expected ',' or ']' in JSON");
        }
        return v;
    }

    std::string parseString() {
        if (peek() != '"') throw std::runtime_error("expected string in JSON");
        ++i_;
        std::string out;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (i_ >= s_.size()) break;
                char e = s_[i_++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = parseHex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        throw std::runtime_error("unterminated string in JSON");
    }

    unsigned parseHex4() {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            if (i_ >= s_.size()) throw std::runtime_error("bad \\u escape");
            char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else throw std::runtime_error("bad \\u escape");
        }
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    Value parseBool() {
        if (peek() == 't') { expect("true"); Value v; v.type = Value::Type::Bool; v.boolean = true; return v; }
        expect("false");
        Value v;
        v.type = Value::Type::Bool;
        v.boolean = false;
        return v;
    }

    Value parseNumber() {
        std::size_t start = i_;
        if (peek() == '-') ++i_;
        bool isDouble = false;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        if (peek() == '.') { isDouble = true; ++i_; while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_; }
        if (peek() == 'e' || peek() == 'E') {
            isDouble = true;
            ++i_;
            if (peek() == '+' || peek() == '-') ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        std::string num = s_.substr(start, i_ - start);
        Value v;
        if (isDouble) {
            v.type = Value::Type::Double;
            v.number = std::stod(num);
        } else {
            v.type = Value::Type::Int;
            v.integer = std::stoll(num);
        }
        return v;
    }
};

inline bool parse(const std::string& text, Value& out) {
    try {
        out = Parser(text).parse();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

inline void dumpInto(std::string& out, const Value& v, int indent, int depth) {
    auto pad = [&](int d) { out.append(d * indent, ' '); };
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Type::Int: out += std::to_string(v.integer); break;
    case Value::Type::Double: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", v.number);
        out += buf;
        break;
    }
    case Value::Type::String: {
        out.push_back('"');
        for (char c : v.str) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
            }
        }
        out.push_back('"');
        break;
    }
    case Value::Type::Array: {
        if (v.array.empty()) { out += "[]"; break; }
        out += "[\n";
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            pad(depth + 1);
            dumpInto(out, v.array[i], indent, depth + 1);
            if (i + 1 < v.array.size()) out.push_back(',');
            out.push_back('\n');
        }
        pad(depth);
        out.push_back(']');
        break;
    }
    case Value::Type::Object: {
        if (v.object.empty()) { out += "{}"; break; }
        out += "{\n";
        for (std::size_t i = 0; i < v.object.size(); ++i) {
            pad(depth + 1);
            out.push_back('"');
            out += v.object[i].first;
            out += "\": ";
            dumpInto(out, v.object[i].second, indent, depth + 1);
            if (i + 1 < v.object.size()) out.push_back(',');
            out.push_back('\n');
        }
        pad(depth);
        out.push_back('}');
        break;
    }
    }
}

inline std::string dump(const Value& v, int indent = 1) {
    std::string out;
    dumpInto(out, v, indent, 0);
    out.push_back('\n');
    return out;
}

}  // namespace json
}  // namespace tw
