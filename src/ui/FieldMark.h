#pragma once

#include <QString>

#include <cstdint>

namespace aiedit::ui {

// A named range of bytes, e.g. a header field the assistant worked out; the hex view colours it
// and shows the name and detail on hover.
struct FieldMark
{
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    QString name;
    QString detail; // value and meaning
};

} // namespace aiedit::ui
