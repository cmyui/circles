#pragma once

#include <circles/types.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace circles {

inline std::string lowercase(std::string_view text) {
  std::string result(text);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) {
                   return std::tolower(c);
                 });
  return result;
}

// Without leading and trailing spaces, tabs and carriage returns.
inline std::string_view trim(std::string_view text) {
  const auto blank = [](char c) {
    return c == ' ' || c == '\t' || c == '\r';
  };
  while (!text.empty() && blank(text.front()))
    text.remove_prefix(1);
  while (!text.empty() && blank(text.back()))
    text.remove_suffix(1);
  return text;
}

inline std::vector<std::string_view> split(std::string_view text,
                                           char             separator) {
  std::vector<std::string_view> fields;
  if (text.empty())
    return fields;
  for (size_t start = 0;;) {
    const size_t end = text.find(separator, start);
    fields.push_back(text.substr(start, end - start));
    if (end == text.npos)
      return fields;
    start = end + 1;
  }
}

// An integer field of a separated list; 0 if empty or malformed.
inline i32 field_value(std::string_view field) {
  i32        value = 0;
  const auto end = field.data() + field.size();
  const auto [last, error] = std::from_chars(field.data(), end, value);
  return error == std::errc() && last == end ? value : 0;
}

}  // namespace circles
