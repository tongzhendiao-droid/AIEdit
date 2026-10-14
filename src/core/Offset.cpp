#include "core/Offset.h"

#include <algorithm>
#include <limits>

namespace cppedit::core {

namespace {

std::string_view trim(std::string_view s)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

int digitValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::optional<std::uint64_t> parseNumber(std::string_view digits, unsigned base)
{
    if (digits.empty())
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : digits) {
        const int d = digitValue(c);
        if (d < 0 || static_cast<unsigned>(d) >= base)
            return std::nullopt;
        if (value > (std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(d)) / base)
            return std::nullopt;
        value = value * base + static_cast<unsigned>(d);
    }
    return value;
}

} // namespace

std::optional<std::uint64_t> parseOffset(std::string_view text, std::uint64_t current)
{
    text = trim(text);
    int sign = 0;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        sign = text.front() == '+' ? 1 : -1;
        text = trim(text.substr(1));
    }

    unsigned base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    } else if (text.size() > 1 && (text.back() == 'h' || text.back() == 'H')) {
        base = 16;
        text.remove_suffix(1);
    } else if (std::any_of(text.begin(), text.end(), [](char c) { return digitValue(c) >= 10; })) {
        base = 16;
    }

    const auto value = parseNumber(text, base);
    if (!value)
        return std::nullopt;
    if (sign > 0) {
        if (*value > std::numeric_limits<std::uint64_t>::max() - current)
            return std::nullopt;
        return current + *value;
    }
    if (sign < 0)
        return *value > current ? 0 : current - *value;
    return value;
}

} // namespace cppedit::core
