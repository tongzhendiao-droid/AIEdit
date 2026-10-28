#include "core/Epl.h"

#include <QTest>

using namespace aiedit::core::epl;

namespace {

// Writes 易语言's binary layouts, little-endian, to build small source files.
class Builder
{
public:
    Builder& i8(int v)
    {
        bytes.push_back(static_cast<char>(v));
        return *this;
    }
    Builder& i16(int v)
    {
        i8(v & 0xFF);
        return i8((v >> 8) & 0xFF);
    }
    Builder& i32(std::int64_t v)
    {
        i16(static_cast<int>(v & 0xFFFF));
        return i16(static_cast<int>((v >> 16) & 0xFFFF));
    }
    Builder& f64(double v)
    {
        char raw[8];
        std::memcpy(raw, &v, 8);
        bytes.append(raw, 8);
        return *this;
    }
    Builder& raw(const std::string& s)
    {
        bytes += s;
        return *this;
    }
    Builder& lengthPrefixed(const std::string& s)
    {
        i32(static_cast<std::int64_t>(s.size()));
        return raw(s);
    }
    Builder& cString(const std::string& s)
    {
        raw(s);
        return i8(0);
    }
    Builder& bstr(const std::string& s)
    {
        if (s.empty())
            return i32(0);
        i32(static_cast<std::int64_t>(s.size()) + 1);
        raw(s);
        return i8(0);
    }

    std::string bytes;
};

struct Var
{
    std::int32_t id;
    std::int32_t type;
    int flags;
    std::string name;
    std::string comment;
    std::vector<std::int32_t> bounds;
};

void variables(Builder& b, const std::vector<Var>& vars)
{
    std::vector<std::string> items;
    for (const Var& v : vars) {
        Builder item;
        item.i32(v.type).i16(v.flags).i8(static_cast<int>(v.bounds.size()));
        for (const auto bound : v.bounds)
            item.i32(bound);
        item.cString(v.name).cString(v.comment);
        Builder withLength;
        withLength.lengthPrefixed(item.bytes);
        items.push_back(withLength.bytes);
    }
    std::size_t size = vars.size() * 8;
    for (const auto& item : items)
        size += item.size();
    b.i32(static_cast<std::int64_t>(vars.size())).i32(static_cast<std::int64_t>(size));
    for (const Var& v : vars)
        b.i32(v.id);
    std::size_t offset = 0;
    for (const auto& item : items) {
        b.i32(static_cast<std::int64_t>(offset));
        offset += item.size();
    }
    for (const auto& item : items)
        b.raw(item);
}

void section(Builder& file, std::int32_t key, const std::string& data)
{
    file.i32(0x15117319).i32(0).i32(key).raw(std::string(30, '\0')).i16(0).i32(0).i32(0).i32(0);
    file.i32(static_cast<std::int64_t>(data.size())).raw(std::string(40, '\0')).raw(data);
}

// A call's head: the command, its library (0 is krnln, -2 the user's own), flags, what the IDE
// could not read, and the comment.
void callHead(Builder& b, std::int32_t command, int library, int flags = 0, const std::string& comment = {})
{
    b.i32(command).i16(library).i16(flags).bstr({}).bstr(comment);
}

constexpr std::int32_t kMethod = 0x04000001;
constexpr std::int32_t kClass = 0x09000001;
constexpr std::int32_t kLocalX = 0x25000001;
constexpr std::int32_t kLocalI = 0x25000002;
constexpr std::int32_t kParameter = 0x25000003;
constexpr std::int32_t kGlobal = 0x05000001;
constexpr std::int32_t kConstant = 0x18000001;
constexpr std::int32_t kInt = static_cast<std::int32_t>(0x80000301);
constexpr std::int32_t kText = static_cast<std::int32_t>(0x80000004);
// 测试, in GBK.
const std::string kTest = "\xB2\xE2\xCA\xD4";

std::string code()
{
    Builder c;
    // 信息框 (“你好”, 0, )
    c.i8(0x6A);
    callHead(c, 192, 0);
    c.i8(0x36).i8(0x1A).bstr("hello").i8(0x3B).i32(0).i8(0x16).i8(0x01);
    // x ＝ 1.5 ＋ x × 2
    c.i8(0x6A);
    callHead(c, 52, 0);
    c.i8(0x36).i8(0x38).i32(kLocalX).i8(0x37);
    c.i8(0x21);
    callHead(c, 19, 0);
    c.i8(0x36).i8(0x17).f64(1.5);
    c.i8(0x21);
    callHead(c, 15, 0);
    c.i8(0x36).i8(0x38).i32(kLocalX).i8(0x37).i8(0x3B).i32(2).i8(0x01);
    c.i8(0x01).i8(0x01);
    // .如果真 (x ＝ 1)  ' 测试
    c.i8(0x6C);
    callHead(c, 1, 0, 0, kTest);
    c.i8(0x36).i8(0x21);
    callHead(c, 38, 0);
    c.i8(0x36).i8(0x38).i32(kLocalX).i8(0x37).i8(0x3B).i32(1).i8(0x01).i8(0x01);
    //     ' only a comment
    c.i8(0x6A);
    callHead(c, 0, -1, 0, "only a comment");
    c.i8(0x36).i8(0x01);
    c.i8(0x52).i8(0x73);
    // .计次循环首 (3, i)
    c.i8(0x70);
    callHead(c, 7, 0);
    c.i8(0x36).i8(0x3B).i32(3).i8(0x38).i32(kLocalI).i8(0x37).i8(0x01);
    //     ' 子程序1 (i [2], #PI, 真)   (commented out)
    c.i8(0x6A);
    callHead(c, kMethod, -2, 0x20);
    c.i8(0x36).i8(0x38).i32(kLocalI).i8(0x3A).i8(0x3B).i32(2).i8(0x37).i8(0x1B).i32(kConstant).i8(0x18).i16(1).i8(0x01);
    c.i8(0x55).i8(0x71);
    callHead(c, 8, 0);
    c.i8(0x36).i8(0x01);
    // .判断开始 (假) / .默认 / .判断结束
    c.i8(0x6D).i8(0x6E);
    callHead(c, 2, 0);
    c.i8(0x36).i8(0x18).i16(0).i8(0x01);
    c.i8(0x53).i8(0x6F).i8(0x54).i8(0x74);
    // 返回 ({ 1, 2 })
    c.i8(0x6A);
    callHead(c, 13, 0);
    c.i8(0x36).i8(0x1F).i8(0x3B).i32(1).i8(0x3B).i32(2).i8(0x20).i8(0x01);
    return c.bytes;
}

std::string sampleFile()
{
    Builder system;
    system.i16(5).i16(6).i32(1).i32(1).i16(1).i16(7).i32(1).i32(0).i32(0).raw(std::string(32, '\0'));

    Builder codeSection;
    codeSection.i32(0x10000).i32(51113791);
    codeSection.i32(4).i32(0);  // commands the library must have
    codeSection.i32(2).i16(0);  // data types
    codeSection.i32(2).i16(0);  // constants
    codeSection.i16(1).lengthPrefixed("krnln\r{d09f2340-8185-11d3-96f6-aaf844c7e325}\r5\r7\r" + kTest);
    codeSection.i32(0).i32(0);  // flags, the main method
    codeSection.i32(0);         // icon
    codeSection.lengthPrefixed({});
    // One class, holding the method.
    codeSection.i32(8).i32(kClass).i32(0);
    codeSection.i32(0).i32(0).lengthPrefixed("Main").lengthPrefixed("the class").i32(4).i32(kMethod);
    variables(codeSection, {});
    // The method.
    codeSection.i32(8).i32(kMethod).i32(0);
    codeSection.i32(kClass).i32(8).i32(kInt).lengthPrefixed("Run").lengthPrefixed({});
    variables(codeSection, {{kLocalX, kInt, 0, "x", {}, {}}, {kLocalI, kInt, 1, "i", kTest, {0}}, {0x25000009, kInt, 0, "", {}, {}}});
    variables(codeSection, {{kParameter, kText, 4 | 2, "p", {}, {}}});
    for (int i = 0; i < 5; ++i)
        codeSection.lengthPrefixed({});
    codeSection.lengthPrefixed(code());
    variables(codeSection, {{kGlobal, kText, 0x100, "g", "a global", {3, 4}}});
    codeSection.i32(0); // structs
    codeSection.i32(0); // DLL commands

    Builder resources;
    resources.i32(0); // forms
    Builder constant;
    constant.i16(2).cString("PI").cString({}).i8(23).f64(3.14);
    Builder withLength;
    withLength.lengthPrefixed(constant.bytes);
    resources.i32(1).i32(static_cast<std::int64_t>(8 + withLength.bytes.size())).i32(kConstant).i32(0).raw(withLength.bytes);

    Builder file;
    file.raw("CNWTEPRG");
    section(file, 0x02007319, system.bytes);
    section(file, 0x04007319, resources.bytes);
    section(file, 0x03007319, codeSection.bytes);
    section(file, 0x07007319, {});
    return file.bytes;
}

std::span<const std::uint8_t> span(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string decodeGbkForTest(std::string_view s)
{
    std::string out(s);
    const std::string test = kTest;
    for (std::size_t at; (at = out.find(test)) != std::string::npos;)
        out.replace(at, test.size(), "测试");
    return out;
}

} // namespace

class TestEpl : public QObject
{
    Q_OBJECT

private slots:
    void parsesDeclarations();
    void writesText();
    void encrypted();
    void brokenFiles();
    void removedNames();
};

void TestEpl::parsesDeclarations()
{
    const std::string file = sampleFile();
    const Project p = parse(span(file));
    QCOMPARE(p.status, Project::Status::Ok);
    QCOMPARE(p.language, 1);
    QCOMPARE(p.libraries.size(), 1u);
    QCOMPARE(p.libraries[0].fileName, std::string("krnln"));
    QCOMPARE(p.libraries[0].major, 5);
    QCOMPARE(p.classes.size(), 1u);
    QCOMPARE(p.methods.size(), 1u);
    QCOMPARE(p.methods[0].locals.size(), 3u);
    QCOMPARE(p.globals.size(), 1u);
    QCOMPARE(p.constants.size(), 1u);
    QCOMPARE(p.constants[0].number, 3.14);
}

void TestEpl::writesText()
{
    const std::string file = sampleFile();
    std::size_t unreadable = 99;
    const std::string text = toText(parse(span(file)), decodeGbkForTest, &unreadable);
    QCOMPARE(unreadable, 0u);
    const std::string expected = ".版本 2\n"
                                 "\n"
                                 ".程序集 Main, , , the class\n"
                                 "\n"
                                 ".子程序 Run, 整数型, 公开\n"
                                 ".参数 p, 文本型, 参考 可空\n"
                                 ".局部变量 x, 整数型\n"
                                 ".局部变量 i, 整数型, 静态, \"0\", 测试\n"
                                 "\n"
                                 "信息框 (“hello”, 0, )\n"
                                 "x ＝ 1.5 ＋ x × 2\n"
                                 ".如果真 (x ＝ 1)  ' 测试\n"
                                 "    ' only a comment\n"
                                 ".如果真结束\n"
                                 ".计次循环首 (3, i)\n"
                                 "    ' Run (i [2], #PI, 真)\n"
                                 ".计次循环尾 ()\n"
                                 ".判断开始 (假)\n"
                                 "\n"
                                 ".默认\n"
                                 "\n"
                                 ".判断结束\n"
                                 "返回 ({ 1, 2 })\n"
                                 "\n"
                                 ".全局变量 g, 文本型, 公开, \"3,4\", a global\n"
                                 "\n"
                                 ".常量 PI, \"3.14\", 公开\n";
    QCOMPARE(QString::fromStdString(text), QString::fromStdString(expected));
}

void TestEpl::encrypted()
{
    const std::string file = std::string("WTLE") + std::string(12, '\x01');
    QCOMPARE(parse(span(file)).status, Project::Status::Encrypted);
    QCOMPARE(parse(span(std::string("not epl at all"))).status, Project::Status::NotEpl);
}

void TestEpl::brokenFiles()
{
    const std::string file = sampleFile();
    // Cut anywhere, or with a byte changed anywhere: whatever can be read, never past the end.
    for (std::size_t size = 0; size < file.size(); size += 7) {
        const std::string cut = file.substr(0, size);
        const Project p = parse(span(cut));
        toText(p, decodeGbkForTest);
    }
    for (std::size_t at = 8; at < file.size(); at += 5) {
        std::string changed = file;
        changed[at] = static_cast<char>(changed[at] ^ 0xA5);
        toText(parse(span(changed)), decodeGbkForTest);
    }
    const Project cut = parse(span(file.substr(0, file.size() / 2)));
    QCOMPARE(cut.status, Project::Status::Broken);
}

void TestEpl::removedNames()
{
    // Code that still refers to a control removed and made anew: the IDE keeps its old name in
    // the section of things removed, and so does the text.
    std::string file = sampleFile();
    const std::string end = file.substr(file.size() - (3 * 4 + 30 + 2 + 3 * 4 + 4 + 40)); // the last section, which is empty
    file.resize(file.size() - end.size());
    Builder losable;
    losable.lengthPrefixed("out.exe");
    Builder names;
    names.cString("old_name");
    losable.i32(1).i32(static_cast<std::int64_t>(12 + names.bytes.size())).i32(kLocalI).i32(0).i32(0).raw(names.bytes);
    Builder withSection;
    withSection.raw(file);
    section(withSection, 0x05007319, losable.bytes);
    withSection.raw(end);
    const Project p = parse(span(withSection.bytes));
    QCOMPARE(p.status, Project::Status::Ok);
    QCOMPARE(p.removedNames.size(), 1u);
    QCOMPARE(p.removedNames[0].second, std::string("old_name"));
    // A name still in use wins: i keeps its name.
    QVERIFY(toText(p, decodeGbkForTest).find("old_name") == std::string::npos);
}

QTEST_GUILESS_MAIN(TestEpl)
#include "tst_epl.moc"
