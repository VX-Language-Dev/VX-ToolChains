#include "vx/util/str.hpp"

namespace vx::util {

std::string_view trim(std::string_view s) {
  constexpr std::string_view ws = " \t\r\n\v\f";
  const std::size_t begin = s.find_first_not_of(ws);
  if (begin == std::string_view::npos) {
    return {};
  }
  const std::size_t end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}

std::vector<std::string_view> split(std::string_view s, char delim) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t pos = s.find(delim, start);
    if (pos == std::string_view::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

std::string repeat(std::string_view s, std::size_t n) {
  std::string out;
  out.reserve(s.size() * n);
  for (std::size_t i = 0; i < n; ++i) {
    out.append(s);
  }
  return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) {
    return false;
  }
  return s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  if (suffix.size() > s.size()) {
    return false;
  }
  return s.substr(s.size() - suffix.size()) == suffix;
}

bool contains(std::string_view s, std::string_view substr) {
  if (substr.empty()) {
    return true;
  }
  return s.find(substr) != std::string_view::npos;
}

std::string replace(std::string_view s, std::string_view from,
                    std::string_view to) {
  if (from.empty()) {
    return std::string(s);
  }
  std::string result;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t pos = s.find(from, start);
    if (pos == std::string_view::npos) {
      result.append(s.substr(start));
      break;
    }
    result.append(s.substr(start, pos - start));
    result.append(to);
    start = pos + from.size();
  }
  return result;
}

} // namespace vx::util
