#include "core/EditBuffer.h"

#include <QTest>

#include <string>

using aiedit::core::EditBuffer;
using Merge = EditBuffer::Merge;

namespace {

std::span<const std::uint8_t> bytesOf(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string text(const EditBuffer& buffer)
{
    const auto& content = *buffer.content();
    return {content.begin(), content.end()};
}

std::string text(const std::vector<std::uint8_t>& bytes)
{
    return {bytes.begin(), bytes.end()};
}

} // namespace

class TestEditBuffer : public QObject
{
    Q_OBJECT

private slots:
    void replaceUndoRedo();
    void typingIsOneStep();
    void deletionsMerge();
    void tracksTheSavedState();
    void snapshotsNeverChange();
    void clampsToTheContent();
};

void TestEditBuffer::replaceUndoRedo()
{
    EditBuffer buffer(bytesOf("hello world"));
    QVERIFY(!buffer.canUndo() && !buffer.canRedo());
    buffer.replace(6, 5, bytesOf("there"));
    QCOMPARE(text(buffer), std::string("hello there"));

    const auto undone = buffer.undo();
    QVERIFY(undone);
    QCOMPARE(text(buffer), std::string("hello world"));
    QCOMPARE(undone->offset, 6u);
    QCOMPARE(text(undone->inserted), std::string("world")); // what came back
    QCOMPARE(text(undone->removed), std::string("there"));
    QVERIFY(!buffer.canUndo() && buffer.canRedo());

    const auto redone = buffer.redo();
    QVERIFY(redone);
    QCOMPARE(text(buffer), std::string("hello there"));
    QCOMPARE(text(redone->inserted), std::string("there"));
    QVERIFY(!buffer.redo());
}

void TestEditBuffer::typingIsOneStep()
{
    EditBuffer buffer(bytesOf("ab"));
    buffer.replace(1, 0, bytesOf("x"), Merge::Typing);
    buffer.replace(2, 0, bytesOf("y"), Merge::Typing);
    buffer.replace(3, 0, bytesOf("z"), Merge::Typing);
    QCOMPARE(text(buffer), std::string("axyzb"));
    buffer.undo();
    QCOMPARE(text(buffer), std::string("ab"));
    QVERIFY(!buffer.canUndo());

    // Typing over a selection, then on: undone together, the selection comes back.
    buffer.redo();
    buffer.replace(1, 3, bytesOf("1"), Merge::Typing);
    buffer.replace(2, 0, bytesOf("2"), Merge::Typing);
    QCOMPARE(text(buffer), std::string("a12b"));
    buffer.undo();
    QCOMPARE(text(buffer), std::string("axyzb"));

    // A move of the caret in between makes two steps; so does typing somewhere else.
    buffer.replace(5, 0, bytesOf("!"), Merge::Typing);
    buffer.breakMerge();
    buffer.replace(6, 0, bytesOf("?"), Merge::Typing);
    buffer.replace(0, 0, bytesOf(">"), Merge::Typing);
    buffer.undo();
    QCOMPARE(text(buffer), std::string("axyzb!?"));
    buffer.undo();
    QCOMPARE(text(buffer), std::string("axyzb!"));
}

void TestEditBuffer::deletionsMerge()
{
    EditBuffer buffer(bytesOf("0123456789"));
    buffer.replace(5, 1, {}, Merge::DeleteBackward); // Backspace after "5"
    buffer.replace(4, 1, {}, Merge::DeleteBackward);
    buffer.replace(3, 1, {}, Merge::DeleteBackward);
    QCOMPARE(text(buffer), std::string("0126789"));
    buffer.replace(3, 1, {}, Merge::DeleteForward); // Delete, a different kind: a new step
    buffer.replace(3, 1, {}, Merge::DeleteForward);
    QCOMPARE(text(buffer), std::string("01289"));
    const auto undone = buffer.undo();
    QCOMPARE(text(undone->inserted), std::string("67"));
    QCOMPARE(text(buffer), std::string("0126789"));
    buffer.undo();
    QCOMPARE(text(buffer), std::string("0123456789"));
    QVERIFY(!buffer.canUndo());
}

void TestEditBuffer::tracksTheSavedState()
{
    EditBuffer buffer(bytesOf("abc"));
    QVERIFY(!buffer.isModified());
    buffer.replace(3, 0, bytesOf("d"), Merge::Typing);
    QVERIFY(buffer.isModified());
    buffer.undo();
    QVERIFY(!buffer.isModified()); // back to what is on disk
    buffer.redo();
    buffer.setSaved();
    QVERIFY(!buffer.isModified());

    // Typing on after a save is a new step, so undoing it returns to the saved state.
    buffer.replace(4, 0, bytesOf("e"), Merge::Typing);
    QVERIFY(buffer.isModified());
    buffer.undo();
    QVERIFY(!buffer.isModified());
    QCOMPARE(text(buffer), std::string("abcd"));

    // Undo past the saved state and change something else: the saved state cannot come back.
    buffer.undo();
    buffer.replace(0, 0, bytesOf("x"));
    buffer.undo();
    QCOMPARE(text(buffer), std::string("abc"));
    QVERIFY(buffer.isModified());
}

void TestEditBuffer::snapshotsNeverChange()
{
    EditBuffer buffer(bytesOf("before"));
    const auto snapshot = buffer.content();
    buffer.replace(0, 6, bytesOf("after"));
    QCOMPARE(text(*snapshot), std::string("before")); // still readable by whoever holds it
    QCOMPARE(text(buffer), std::string("after"));
}

void TestEditBuffer::clampsToTheContent()
{
    EditBuffer buffer(bytesOf("abc"));
    buffer.replace(2, 100, bytesOf("Z"));
    QCOMPARE(text(buffer), std::string("abZ"));
    buffer.replace(100, 5, bytesOf("!"));
    QCOMPARE(text(buffer), std::string("abZ!"));
    buffer.replace(1, 0, {}); // nothing to do: not a step
    buffer.undo();
    buffer.undo();
    QCOMPARE(text(buffer), std::string("abc"));
    QVERIFY(!buffer.canUndo());
}

QTEST_APPLESS_MAIN(TestEditBuffer)
#include "tst_editbuffer.moc"
