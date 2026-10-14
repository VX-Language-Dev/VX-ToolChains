#include "json_utils.h"
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace json {

static void skip_ws(const std::string& s, size_t& pos) {
    while (pos < s.size() && std::isspace(s[pos])) pos++;
}

static Value parse_value(const std::string& s, size_t& pos);

static Value parse_string(const std::string& s, size_t& pos) {
    pos++; // skip opening '"'
    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\') {
            pos++;
            switch (s[pos]) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                default: result += s[pos]; break;
            }
            pos++;
        } else {
            result += s[pos];
            pos++;
        }
    }
    if (pos < s.size()) pos++; // skip closing '"'
    return Value(result);
}

static Value parse_number(const std::string& s, size_t& pos) {
    size_t start = pos;
    while (pos < s.size() && (std::isdigit(s[pos]) || s[pos] == '.' || s[pos] == '-' || s[pos] == '+')) pos++;
    std::string num_str = s.substr(start, pos - start);
    if (num_str.find('.') != std::string::npos) {
        return Value(std::stod(num_str));
    }
    return Value(std::stoi(num_str));
}

static Value parse_array(const std::string& s, size_t& pos) {
    pos++; // skip '['
    Value::Array arr;
    skip_ws(s, pos);
    if (pos < s.size() && s[pos] == ']') { pos++; return Value(arr); }
    while (true) {
        arr.push_back(parse_value(s, pos));
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') { pos++; skip_ws(s, pos); }
        else break;
    }
    pos++; // skip ']'
    return Value(arr);
}

static Value parse_object(const std::string& s, size_t& pos) {
    pos++; // skip '{'
    Value::Object obj;
    skip_ws(s, pos);
    if (pos < s.size() && s[pos] == '}') { pos++; return Value(obj); }
    while (true) {
        skip_ws(s, pos);
        std::string key = parse_string(s, pos).as_string();
        skip_ws(s, pos);
        pos++; // skip ':'
        Value val = parse_value(s, pos);
        obj[key] = val;
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') { pos++; skip_ws(s, pos); }
        else break;
    }
    pos++; // skip '}'
    return Value(obj);
}

static Value parse_value(const std::string& s, size_t& pos) {
    skip_ws(s, pos);
    if (pos >= s.size()) return Value();
    if (s[pos] == '"') return parse_string(s, pos);
    if (s[pos] == '[') return parse_array(s, pos);
    if (s[pos] == '{') return parse_object(s, pos);
    if (s[pos] == 't' && s.substr(pos, 4) == "true") { pos += 4; return Value(true); }
    if (s[pos] == 'f' && s.substr(pos, 5) == "false") { pos += 5; return Value(false); }
    if (s[pos] == 'n' && s.substr(pos, 4) == "null") { pos += 4; return Value(); }
    if (std::isdigit(s[pos]) || s[pos] == '-') return parse_number(s, pos);
    return Value();
}

Value Value::parse(const std::string& str) {
    size_t pos = 0;
    return parse_value(str, pos);
}

std::string json_escape(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

std::string Value::serialize(const Value& val) {
    switch (val.type_) {
        case Type::Null: return "null";
        case Type::Bool: return val.bool_ ? "true" : "false";
        case Type::Int: return std::to_string(val.int_);
        case Type::Float: return std::to_string(val.float_);
        case Type::String: return "\"" + json_escape(val.string_) + "\"";
        case Type::Array: {
            std::string s = "[";
            for (size_t i = 0; i < val.array_.size(); i++) {
                if (i > 0) s += ",";
                s += serialize(val.array_[i]);
            }
            return s + "]";
        }
        case Type::Object: {
            std::string s = "{";
            size_t i = 0;
            for (const auto& [k, v] : val.object_) {
                if (i > 0) s += ",";
                s += "\"" + json_escape(k) + "\":" + serialize(v);
                i++;
            }
            return s + "}";
        }
        default: return "null";
    }
}

LSPMessage LSPMessage::parse(const std::string& json_str) {
    LSPMessage msg;
    Value root = Value::parse(json_str);
    if (root.type() == Value::Type::Object) {
        const auto& obj = root.as_object();
        if (obj.count("id") && obj.at("id").type() == Value::Type::Int)
            msg.id = static_cast<int>(obj.at("id").as_int());
        else if (obj.count("id") && obj.at("id").type() == Value::Type::Float)
            msg.id = static_cast<int>(obj.at("id").as_float());
        if (obj.count("jsonrpc")) msg.jsonrpc = obj.at("jsonrpc").as_string();
        if (obj.count("method")) msg.method = obj.at("method").as_string();
        if (obj.count("params") && obj.at("params").type() == Value::Type::Object)
            msg.params = obj.at("params");
        if (obj.count("result")) msg.result = obj.at("result");
    }
    return msg;
}

std::string LSPMessage::serialize() const {
    Value::Object obj;
    obj["jsonrpc"] = Value(std::string("2.0"));
    if (id) obj["id"] = Value(*id);
    obj["method"] = Value(method);
    if (!params.is_null()) obj["params"] = params;
    if (!result.is_null()) obj["result"] = result;
    return Value::serialize(Value(obj));
}

std::optional<std::string> read_lsp_message() {
    static std::string buffer;
    std::string line;
    
    // Read headers until empty line
    int content_length = 0;
    while (std::getline(std::cin, line)) {
        if (line.empty()) break;
        if (line.rfind("Content-Length:", 0) == 0) {
            content_length = std::stoi(line.substr(16));
        }
    }
    
    if (content_length <= 0) return std::nullopt;
    
    std::string body(content_length, '\0');
    std::cin.read(&body[0], content_length);
    
    return body;
}

void write_lsp_message(const std::string& json_str) {
    std::cout << "Content-Length: " << json_str.size() << "\r\n\r\n" << json_str << std::flush;
}

} // namespace json
