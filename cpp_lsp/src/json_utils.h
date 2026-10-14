#pragma once
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <iomanip>
#include <optional>
#include <variant>
#include <iostream>

// Simple JSON value type for LSP messages
namespace json {

class Value {
public:
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;

    enum class Type { Null, Bool, Int, Float, String, Array, Object };

    Value() : type_(Type::Null) {}
    explicit Value(bool v) : type_(Type::Bool), bool_(v) {}
    explicit Value(int v) : type_(Type::Int), int_(v) {}
    explicit Value(double v) : type_(Type::Float), float_(v) {}
    explicit Value(std::string v) : type_(Type::String), string_(std::move(v)) {}
    explicit Value(Array v) : type_(Type::Array), array_(std::move(v)) {}
    explicit Value(Object v) : type_(Type::Object), object_(std::move(v)) {}

    Type type() const { return type_; }

    bool as_bool() const { return bool_; }
    int as_int() const { return int_; }
    double as_float() const { return float_; }
    const std::string& as_string() const { return string_; }
    const Array& as_array() const { return array_; }
    const Object& as_object() const { return object_; }

    Value& operator[](const std::string& key) { return object_[key]; }
    const Value& operator[](const std::string& key) const { return object_.at(key); }

    bool contains(const std::string& key) const { return object_.count(key) > 0; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    static Value parse(const std::string& str);
    static std::string serialize(const Value& val);

private:
    Type type_;
    bool bool_ = false;
    int int_ = 0;
    double float_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

// LSP Message wrapper
struct LSPMessage {
    std::optional<int> id;
    std::string jsonrpc = "2.0";
    std::string method;
    Value params;
    Value result;
    std::optional<Value> error;

    static LSPMessage parse(const std::string& json_str);
    std::string serialize() const;
};

// LSP protocol helper: read message from stdin (Content-Length format)
std::optional<std::string> read_lsp_message();
void write_lsp_message(const std::string& json_str);

// Free serialize function in json namespace
inline std::string serialize(const Value& val) { return Value::serialize(val); }
inline std::string json_escape(const std::string& s);

} // namespace json
