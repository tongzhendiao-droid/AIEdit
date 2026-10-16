#include "core/ByteDiff.h"
#include "core/Diff.h"

#include <QElapsedTimer>
#include <QTest>

#include <random>
#include <string>
#include <vector>

using namespace aiedit::core;

namespace {

using Keys = std::vector<std::uint64_t>;
using Hunks = std::vector<DiffHunk>;

// Each character stands for a line.
Keys keys(std::string_view lines)
{
    return {lines.begin(), lines.end()};
}

// Whether the hunks turn the left side into the right one: in order, not empty, and with equal
// lines between them.
bool reproduces(const Keys& left, const Keys& right, const Hunks& hunks)
{
    std::size_t i = 0;
    std::size_t j = 0;
    for (const DiffHunk& hunk : hunks) {
        if (hunk.leftStart < i || hunk.rightStart < j || hunk.leftStart - i != hunk.rightStart - j)
            return false;
        if (hunk.leftCount == 0 && hunk.rightCount == 0)
            return false;
        for (; i < hunk.leftStart; ++i, ++j)
            if (left[i] != right[j])
                return false;
        i += hunk.leftCount;
        j += hunk.rightCount;
    }
    if (i > left.size() || j > right.size() || left.size() - i != right.size() - j)
        return false;
    for (; i < left.size(); ++i, ++j)
        if (left[i] != right[j])
            return false;
    return true;
}

std::size_t changedLines(const Hunks& hunks)
{
    std::size_t changed = 0;
    for (const DiffHunk& hunk : hunks)
        changed += hunk.leftCount + hunk.rightCount;
    return changed;
}

std::size_t longestCommonSubsequence(const Keys& a, const Keys& b)
{
    std::vector<std::vector<std::size_t>> table(a.size() + 1, std::vector<std::size_t>(b.size() + 1, 0));
    for (std::size_t i = 1; i <= a.size(); ++i)
        for (std::size_t j = 1; j <= b.size(); ++j)
            table[i][j] = a[i - 1] == b[j - 1] ? table[i - 1][j - 1] + 1 : std::max(table[i - 1][j], table[i][j - 1]);
    return table[a.size()][b.size()];
}

} // namespace

class TestDiff : public QObject
{
    Q_OBJECT

private slots:
    void basics();
    void minimalOnSmallInputs();
    void slidesBlocksDown();
    void keepsTheClosingLineOfAChange();
    void bigFilesWithFewChanges();
    void filesThatDifferEverywhere();
    void cancels();
    void inlineWords();
    void inlineCjk();
    void inlineMarksWholeLinesWithLittleInCommon();
    void bytes();
    void bytesShiftedByAnInsertion();
};

void TestDiff::basics()
{
    QVERIFY(diffSequences(keys(""), keys("")).empty());
    QVERIFY(diffSequences(keys("abc"), keys("abc")).empty());
    QVERIFY((diffSequences(keys("abc"), keys("abxc")) == Hunks{{2, 0, 2, 1}}));
    QVERIFY((diffSequences(keys("abc"), keys("ac")) == Hunks{{1, 1, 1, 0}}));
    QVERIFY((diffSequences(keys("abc"), keys("axc")) == Hunks{{1, 1, 1, 1}}));
    QVERIFY((diffSequences(keys(""), keys("ab")) == Hunks{{0, 0, 0, 2}}));
    QVERIFY((diffSequences(keys("ab"), keys("")) == Hunks{{0, 2, 0, 0}}));
    QVERIFY((diffSequences(keys("abcdef"), keys("xbcdey")) == Hunks{{0, 1, 0, 1}, {5, 1, 5, 1}}));
    const Keys left = keys("the quick brown fox");
    const Keys right = keys("a quick red fox jumps");
    const Hunks hunks = diffSequences(left, right);
    QVERIFY(reproduces(left, right, hunks));
}

void TestDiff::minimalOnSmallInputs()
{
    // Against a brute-force longest common subsequence: inputs this small never hit the limit
    // on the search, so the script must be the shortest.
    std::mt19937 random(42);
    std::uniform_int_distribution<int> length(0, 40);
    std::uniform_int_distribution<int> letter(0, 3);
    for (int run = 0; run < 500; ++run) {
        Keys left(static_cast<std::size_t>(length(random)));
        Keys right(static_cast<std::size_t>(length(random)));
        for (std::uint64_t& key : left)
            key = static_cast<std::uint64_t>(letter(random));
        for (std::uint64_t& key : right)
            key = static_cast<std::uint64_t>(letter(random));
        const Hunks hunks = diffSequences(left, right);
        QVERIFY2(reproduces(left, right, hunks), qPrintable(QStringLiteral("run %1").arg(run)));
        QCOMPARE(changedLines(hunks), left.size() + right.size() - 2 * longestCommonSubsequence(left, right));
    }
}

void TestDiff::slidesBlocksDown()
{
    // "ab" was added after "ab" (or before it; both are shortest): it goes after.
    const Keys left = keys("x1ab2y");
    const Keys right = keys("z1abab2w");
    const Hunks hunks = diffSequences(left, right);
    QVERIFY(reproduces(left, right, hunks));
    QVERIFY((hunks == Hunks{{0, 1, 0, 1}, {4, 0, 4, 2}, {5, 1, 7, 1}}));
}

void TestDiff::keepsTheClosingLineOfAChange()
{
    // Lines 1-2 changed, then a function removed: "}" (3) stays with the changed function, and the
    // removed one takes the "}" after it.
    const Keys left = keys("{ab}_f{c}");
    const Keys right = keys("{xy}");
    const Hunks hunks = diffSequences(left, right);
    QVERIFY(reproduces(left, right, hunks));
    QVERIFY((hunks == Hunks{{1, 2, 1, 2}, {4, 5, 4, 0}}));
}

void TestDiff::bigFilesWithFewChanges()
{
    // A million lines, a hundred changes spread through them.
    constexpr std::uint64_t kLines = 1'000'000;
    Keys left(kLines);
    for (std::uint64_t i = 0; i < kLines; ++i)
        left[i] = i % 50'000 == 7 ? 0 : i; // some lines repeat, like blank lines do
    Keys right = left;
    for (std::uint64_t i = 0; i < 100; ++i) {
        const std::uint64_t at = 5'000 + i * 9'973;
        switch (i % 3) {
        case 0:
            right[at] = 10'000'000 + i; // changed
            break;
        case 1:
            right.insert(right.begin() + static_cast<std::ptrdiff_t>(at), 20'000'000 + i); // added
            break;
        default:
            right.erase(right.begin() + static_cast<std::ptrdiff_t>(at)); // removed
            break;
        }
    }
    QElapsedTimer timer;
    timer.start();
    const Hunks hunks = diffSequences(left, right);
    const qint64 elapsed = timer.elapsed();
    qInfo("a million lines, 100 changes: %lld ms", static_cast<long long>(elapsed));
    QVERIFY(reproduces(left, right, hunks));
    QCOMPARE(hunks.size(), 100u);
    QVERIFY(elapsed < 20'000); // generous for unoptimized builds; a release build takes well under a second
}

void TestDiff::filesThatDifferEverywhere()
{
    // Nothing in common: no search at all.
    Keys left(200'000);
    Keys right(200'000);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = i;
        right[i] = 1'000'000 + i;
    }
    QVERIFY((diffSequences(left, right) == Hunks{{0, 200'000, 0, 200'000}}));

    // Everything in common with everything else (ten distinct lines in random order): the search
    // is cut short, and the result must still be right.
    std::mt19937 random(7);
    std::uniform_int_distribution<int> letter(0, 9);
    Keys a(30'000);
    Keys b(30'000);
    for (std::uint64_t& key : a)
        key = static_cast<std::uint64_t>(letter(random));
    for (std::uint64_t& key : b)
        key = static_cast<std::uint64_t>(letter(random));
    QElapsedTimer timer;
    timer.start();
    const Hunks hunks = diffSequences(a, b);
    qInfo("30,000 random lines of ten kinds: %lld ms", static_cast<long long>(timer.elapsed()));
    QVERIFY(reproduces(a, b, hunks));
    QVERIFY(timer.elapsed() < 20'000);
}

void TestDiff::cancels()
{
    std::mt19937 random(3);
    std::uniform_int_distribution<int> letter(0, 9);
    Keys a(20'000);
    Keys b(20'000);
    for (std::uint64_t& key : a)
        key = static_cast<std::uint64_t>(letter(random));
    for (std::uint64_t& key : b)
        key = static_cast<std::uint64_t>(letter(random));
    int asked = 0;
    QVERIFY(diffSequences(a, b, [&asked] { return ++asked > 3; }).empty());
    QVERIFY(asked > 3);
}

void TestDiff::inlineWords()
{
    InlineDiff diff = diffInline(u"int count = 10;", u"int total = 10;");
    QVERIFY((diff.left == std::vector<TextRange>{{4, 5}}));
    QVERIFY((diff.right == std::vector<TextRange>{{4, 5}}));

    diff = diffInline(u"hello world", u"hello there world");
    QVERIFY(diff.left.empty());
    QVERIFY((diff.right == std::vector<TextRange>{{6, 6}})); // "there "

    diff = diffInline(u"same", u"same");
    QVERIFY(diff.left.empty() && diff.right.empty());
}

void TestDiff::inlineCjk()
{
    // Chinese has no spaces between words: each character counts on its own.
    const InlineDiff diff = diffInline(u"天下大势，分久必合", u"天下大事，分久必合");
    QVERIFY((diff.left == std::vector<TextRange>{{3, 1}}));
    QVERIFY((diff.right == std::vector<TextRange>{{3, 1}}));
}

void TestDiff::inlineMarksWholeLinesWithLittleInCommon()
{
    const std::u16string_view left = u"the quick brown fox";
    const std::u16string_view right = u"a slow red dog";
    const InlineDiff diff = diffInline(left, right);
    QVERIFY((diff.left == std::vector<TextRange>{{0, static_cast<std::uint32_t>(left.size())}}));
    QVERIFY((diff.right == std::vector<TextRange>{{0, static_cast<std::uint32_t>(right.size())}}));
}

void TestDiff::bytes()
{
    std::vector<std::uint8_t> left(300);
    for (std::size_t i = 0; i < left.size(); ++i)
        left[i] = static_cast<std::uint8_t>(i * 7);
    std::vector<std::uint8_t> right = left;
    ByteComparison same = compareBytes(left, right);
    QVERIFY(same.ranges.empty());
    QCOMPARE(same.differentBytes, 0u);

    right[10] ^= 1;
    right[12] ^= 1; // close to the one before: the same range
    right[100] ^= 1;
    ByteComparison changed = compareBytes(left, right);
    QVERIFY((changed.ranges == std::vector<ByteRange>{{10, 3}, {100, 1}}));
    QCOMPARE(changed.differentBytes, 3u);
    QVERIFY(!changed.shiftAt);

    // The longer side's extra bytes are the last range.
    right = left;
    right.resize(320, 0xAB);
    ByteComparison longer = compareBytes(left, right);
    QVERIFY((longer.ranges == std::vector<ByteRange>{{300, 20}}));
    QCOMPARE(longer.differentBytes, 20u);
    QCOMPARE(longer.rightSize, 320u);

    QVERIFY(compareBytes(left, right, [] { return true; }).cancelled);
}

void TestDiff::bytesShiftedByAnInsertion()
{
    std::mt19937 random(11);
    std::vector<std::uint8_t> left(20'000);
    for (std::uint8_t& byte : left)
        byte = static_cast<std::uint8_t>(random());
    std::vector<std::uint8_t> right = left;
    right.insert(right.begin() + 5'000, 16, 0x90); // 16 bytes inserted
    ByteComparison inserted = compareBytes(left, right);
    QVERIFY(inserted.shiftAt);
    QCOMPARE(*inserted.shiftAt, 5'000u);
    QCOMPARE(inserted.shift, 16);

    ByteComparison removed = compareBytes(right, left); // the other way round: removed
    QVERIFY(removed.shiftAt);
    QCOMPARE(removed.shift, -16);
}

QTEST_APPLESS_MAIN(TestDiff)
#include "tst_diff.moc"
