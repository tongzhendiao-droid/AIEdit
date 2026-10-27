#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The names of what 易语言's support libraries define, by library (its file name, lower case)
// and index, for writing code out as text. Only names known for certain are kept; others are
// written as 〈krnln命令123〉 and the like, which says what they are without guessing.
namespace aiedit::core::epl {

std::string libraryCommandName(std::string_view library, std::int32_t index);
std::string libraryTypeName(std::string_view library, std::int32_t index);
std::string libraryConstantName(std::string_view library, std::int32_t index);
std::string libraryMemberName(std::string_view library, std::int32_t type, std::int32_t index);

} // namespace aiedit::core::epl
