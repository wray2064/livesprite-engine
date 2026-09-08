// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
#pragma once
// ls_json.h — a small, deterministic JSON value used by the serializer.
//
// Deterministic on purpose: object keys are stored in a std::map so output byte
// order never depends on hashing, and numbers are printed with a fixed
// shortest-round-trip format. The serialized document is the source of truth
// for a LiveSprite file, so two saves of the same state must be byte identical.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ls {
namespace json {

class Value {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Value() = default;
    explicit Value(bool value) : type_(Type::Bool), bool_(value) {}
    explicit Value(double value) : type_(Type::Number), number_(value) {}
    explicit Value(std::string value) : type_(Type::String), string_(std::move(value)) {}

    static Value array()  { Value v; v.type_ = Type::Array;  return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool        asBool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double      asNumber(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
    const std::string& asString() const { return string_; }

    std::vector<Value>& items() { return items_; }
    const std::vector<Value>& items() const { return items_; }

    std::map<std::string, Value>& members() { return members_; }
    const std::map<std::string, Value>& members() const { return members_; }

    void push(Value value) {
        type_ = Type::Array;
        items_.push_back(std::move(value));
    }

    Value& operator[](const std::string& key) {
        type_ = Type::Object;
        return members_[key];
    }

    const Value* find(const std::string& key) const {
        auto it = members_.find(key);
        return it == members_.end() ? nullptr : &it->second;
    }

    bool has(const std::string& key) const { return find(key) != nullptr; }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> items_;
    std::map<std::string, Value> members_;
};

std::string dump(const Value& value);
bool parse(const std::string& text, Value& out);

} // namespace json
} // namespace ls
