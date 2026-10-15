#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace aiedit::core {

// One change: the bytes at [offset, offset + removed.size()) became `inserted`.
struct Edit
{
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> removed;
    std::vector<std::uint8_t> inserted;
};

// The bytes of a document being edited, with undo and redo.
//
// The content is kept whole in memory, and every change makes a new immutable snapshot, so the
// rest of AIEdit (the line index, searches running on other threads) keeps working on bytes
// that never change under it. That costs a copy per change, which is why only documents up to
// kMaxEditableSize can be edited.
class EditBuffer
{
public:
    static constexpr std::uint64_t kMaxEditableSize = 16ull * 1024 * 1024;

    // Whether a change continues the one before it, so that both are undone as one step.
    enum class Merge
    {
        No,
        Typing,         // inserted right after the text inserted before
        DeleteBackward, // removed right before the bytes removed before (Backspace)
        DeleteForward,  // removed where the bytes were removed before (Delete)
    };

    explicit EditBuffer(std::span<const std::uint8_t> content);

    std::shared_ptr<const std::vector<std::uint8_t>> content() const { return m_content; }
    std::uint64_t size() const { return m_content->size(); }

    // Replaces [offset, offset + length) with bytes; both are clamped to the content.
    void replace(std::uint64_t offset, std::uint64_t length, std::span<const std::uint8_t> bytes,
                 Merge merge = Merge::No);
    // The next change starts a new undo step (after the caret moved, for instance).
    void breakMerge() { m_lastMerge = Merge::No; }

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    // Reverts the last step. Returns it as it now stands in the content: its `inserted` bytes
    // are the ones restored.
    std::optional<Edit> undo();
    // Applies the last undone step again and returns it.
    std::optional<Edit> redo();

    // Whether the content differs from the state last marked as saved.
    bool isModified() const { return !m_savedAt || *m_savedAt != m_undo.size(); }
    void setSaved();

private:
    void apply(std::uint64_t offset, std::uint64_t length, std::span<const std::uint8_t> bytes);
    bool tryMerge(const Edit& edit, Merge merge);

    std::shared_ptr<const std::vector<std::uint8_t>> m_content;
    std::vector<Edit> m_undo;
    std::vector<Edit> m_redo;
    Merge m_lastMerge = Merge::No;
    // How many steps the undo stack held when saved; nothing when that state is gone for good.
    std::optional<std::size_t> m_savedAt = 0;
};

} // namespace aiedit::core
