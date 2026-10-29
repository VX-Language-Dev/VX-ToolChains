// json.hpp — 极简 JSON 值类型与编解码（vx 工具链 LSP 消息收发共享实现）。
// 覆盖 LSP 所需的 null/bool/number/string/array/object；对象保留插入序。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx::json {

class Value {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default; // Null
  static Value boolean(bool b);
  static Value number(double n);
  static Value string(std::string s);
  static Value array(std::vector<Value> items);
  static Value object(std::vector<std::pair<std::string, Value>> members);

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }

  bool as_bool(bool fallback = false) const;
  double as_number(double fallback = 0.0) const;
  const std::string &as_string() const;
  const std::vector<Value> &as_array() const;
  const std::vector<std::pair<std::string, Value>> &as_object() const;

  // 对象成员查找；不存在或非对象返回 nullptr
  const Value *find(std::string_view key) const;

  // 取字符串成员（不存在或非字符串返回 fallback）
  std::string_view str_or(std::string_view key, std::string_view fallback = {}) const;
  // 取整数成员（number 截断为 int）
  int int_or(std::string_view key, int fallback = 0) const;

  // 序列化为紧凑 JSON 文本
  std::string dump() const;

private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<Value> arr_;
  std::vector<std::pair<std::string, Value>> obj_;

  void dump_to(std::string &out) const;
};

// 解析 JSON 文本；失败返回 std::nullopt（供 LSP 判定非法消息）
std::optional<Value> parse(std::string_view text);

// JSON 字符串字面量转义（含引号），供 dump 与测试使用
std::string escape_string(std::string_view s);

} // namespace vx::json
