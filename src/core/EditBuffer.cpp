#include "core/EditBuffer.h"

#include <algorithm>

namespace aiedit::core {

EditBuffer::EditBuffer(std::span<const std::uint8_t> content)
    : m_content(std::make_shared<const std::vector<std::uint8_t>>(content.begin(), content.end()))
{
}

void EditBuffer::apply(std::uint64_t offset, std::uint64_t length, std::span<const std::uint8_t> bytes)
{
    const std::vector<std::uint8_t>& old = *m_content;
    const auto start = old.begin() + static_cast<std::ptrdiff_t>(offset);
    auto next = std::make_shared<std::vector<std::uint8_t>>();
    next->reserve(old.size() - length + bytes.size());
    next->insert(next->end(), old.begin(), start);
    next->insert(next->end(), bytes.begin(), bytes.end());
    next->insert(next->end(), start + static_cast<std::ptrdiff_t>(length), old.end());
    m_content = std::move(next);
}

void EditBuffer::replace(std::uint64_t offset, std::uint64_t length, std::span<const std::uint8_t> bytes, Merge merge)
{
    offset = std::min<std::uint64_t>(offset, size());
    length = std::min<std::uint64_t>(length, size() - offset);
    if (length == 0 && bytes.empty())
        return;

    Edit edit;
    edit.offset = offset;
    const auto start = m_content->begin() + static_cast<std::ptrdiff_t>(offset);
    edit.removed.assign(start, start + static_cast<std::ptrdiff_t>(length));
    edit.inserted.assign(bytes.begin(), bytes.end());
    apply(offset, length, bytes);

    m_redo.clear();
    if (m_savedAt && *m_savedAt > m_undo.size())
        m_savedAt.reset(); // the saved state was undone, and now it can never be redone
    if (!tryMerge(edit, merge))
        m_undo.push_back(std::move(edit));
    m_lastMerge = merge;
}

bool EditBuffer::tryMerge(const Edit& edit, Merge merge)
{
    if (merge == Merge::No || merge != m_lastMerge || m_undo.empty())
        return false;
    // The step saved last must stay as it was, or isModified() would be wrong.
    if (m_savedAt && *m_savedAt == m_undo.size())
        return false;
    Edit& last = m_undo.back();
    switch (merge) {
    case Merge::Typing:
        if (!edit.removed.empty() || edit.offset != last.offset + last.inserted.size())
            return false;
        last.inserted.insert(last.inserted.end(), edit.inserted.begin(), edit.inserted.end());
        return true;
    case Merge::DeleteBackward:
        if (!edit.inserted.empty() || !last.inserted.empty() || edit.offset + edit.removed.size() != last.offset)
            return false;
        last.removed.insert(last.removed.begin(), edit.removed.begin(), edit.removed.end());
        last.offset = edit.offset;
        return true;
    case Merge::DeleteForward:
        if (!edit.inserted.empty() || !last.inserted.empty() || edit.offset != last.offset)
            return false;
        last.removed.insert(last.removed.end(), edit.removed.begin(), edit.removed.end());
        return true;
    case Merge::No:
        break;
    }
    return false;
}

std::optional<Edit> EditBuffer::undo()
{
    if (m_undo.empty())
        return std::nullopt;
    Edit edit = std::move(m_undo.back());
    m_undo.pop_back();
    apply(edit.offset, edit.inserted.size(), edit.removed);
    m_lastMerge = Merge::No;
    Edit reverted{edit.offset, edit.inserted, edit.removed};
    m_redo.push_back(std::move(edit));
    return reverted;
}

std::optional<Edit> EditBuffer::redo()
{
    if (m_redo.empty())
        return std::nullopt;
    Edit edit = std::move(m_redo.back());
    m_redo.pop_back();
    apply(edit.offset, edit.removed.size(), edit.inserted);
    m_lastMerge = Merge::No;
    m_undo.push_back(edit);
    return edit;
}

void EditBuffer::setSaved()
{
    m_savedAt = m_undo.size();
    m_lastMerge = Merge::No;
}

} // namespace aiedit::core
