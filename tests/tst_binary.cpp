#include "core/BinaryLayout.h"
#include "core/Disassembler.h"

#include <QTest>

#include <cstring>
#include <string>
#include <vector>

using namespace aiedit::core;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put(Bytes& data, std::size_t offset, std::uint64_t value, int size)
{
    if (data.size() < offset + static_cast<std::size_t>(size))
        data.resize(offset + static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i)
        data[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
}

void put(Bytes& data, std::size_t offset, const std::string& text)
{
    if (data.size() < offset + text.size())
        data.resize(offset + text.size());
    std::memcpy(data.data() + offset, text.data(), text.size());
}

// push rbp; mov rbp, rsp; mov eax, 1; pop rbp; ret
const Bytes kCode = {0x55, 0x48, 0x89, 0xE5, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x5D, 0xC3};

// A 64-bit Windows program: the headers and a .text section holding kCode.
Bytes peFile()
{
    Bytes data(0x600, 0);
    put(data, 0, "MZ");
    put(data, 0x3C, 0x80, 4);
    put(data, 0x80, std::string("PE\0\0", 4));
    put(data, 0x84, 0x8664, 2); // x86-64
    put(data, 0x86, 1, 2);      // one section
    put(data, 0x94, 0xF0, 2);   // size of the optional header
    put(data, 0x98, 0x20B, 2);  // PE32+
    put(data, 0x98 + 24, 0x140000000ull, 8);
    const std::size_t section = 0x98 + 0xF0;
    put(data, section, ".text");
    put(data, section + 8, 0x100, 4);
    put(data, section + 12, 0x1000, 4);
    put(data, section + 16, 0x200, 4);
    put(data, section + 20, 0x400, 4);
    put(data, section + 36, 0x60000020, 4);
    std::copy(kCode.begin(), kCode.end(), data.begin() + 0x400);
    return data;
}

// A 64-bit Linux program with sections: none, .text holding kCode, and the name table.
Bytes elfFile()
{
    Bytes data(0x2000, 0);
    put(data, 0, std::string("\x7F" "ELF", 4));
    data[4] = 2; // 64-bit
    data[5] = 1; // little-endian
    data[6] = 1;
    put(data, 16, 2, 2);  // executable
    put(data, 18, 62, 2); // x86-64
    put(data, 40, 0x1800, 8); // section headers
    put(data, 58, 64, 2);
    put(data, 60, 3, 2);
    put(data, 62, 2, 2); // names in section 2
    std::copy(kCode.begin(), kCode.end(), data.begin() + 0x1000);
    const std::string names = std::string("\0.text\0.shstrtab\0", 17);
    put(data, 0x1100, names);
    const auto header = [&](int i) { return static_cast<std::size_t>(0x1800 + 64 * i); };
    put(data, header(1), 1, 4);             // ".text"
    put(data, header(1) + 4, 1, 4);         // PROGBITS
    put(data, header(1) + 8, 0x6, 8);       // ALLOC, EXECINSTR
    put(data, header(1) + 16, 0x401000, 8); // address
    put(data, header(1) + 24, 0x1000, 8);   // offset
    put(data, header(1) + 32, kCode.size(), 8);
    put(data, header(2), 7, 4);       // ".shstrtab"
    put(data, header(2) + 4, 3, 4);   // STRTAB
    put(data, header(2) + 24, 0x1100, 8);
    put(data, header(2) + 32, names.size(), 8);
    return data;
}

} // namespace

class TestBinary : public QObject
{
    Q_OBJECT

private slots:
    void readsPe();
    void readsElf();
    void rejectsOtherFiles();
    void disassemblesAroundAnOffset();
};

void TestBinary::readsPe()
{
    const Bytes data = peFile();
    const std::optional<BinaryLayout> layout = readBinaryLayout(data);
    QVERIFY(layout);
    QCOMPARE(layout->format, std::string("PE32+"));
    QCOMPARE(layout->architecture, Architecture::X86_64);
    QCOMPARE(layout->imageBase, 0x140000000ull);
    QCOMPARE(layout->sections.size(), 1u);
    const Section& text = layout->sections[0];
    QCOMPARE(text.name, std::string(".text"));
    QCOMPARE(text.fileOffset, 0x400u);
    QCOMPARE(text.address, 0x140001000ull);
    QVERIFY(text.executable);
    QCOMPARE(layout->sectionAt(0x404), &text);
    QVERIFY(!layout->sectionAt(0x10)); // the headers
}

void TestBinary::readsElf()
{
    const Bytes data = elfFile();
    const std::optional<BinaryLayout> layout = readBinaryLayout(data);
    QVERIFY(layout);
    QCOMPARE(layout->format, std::string("ELF64"));
    QCOMPARE(layout->architecture, Architecture::X86_64);
    QCOMPARE(layout->sections.size(), 2u);
    QCOMPARE(layout->sections[0].name, std::string(".text"));
    QCOMPARE(layout->sections[0].address, 0x401000u);
    QVERIFY(layout->sections[0].executable);
    QCOMPARE(layout->sections[1].name, std::string(".shstrtab"));
    QVERIFY(!layout->sections[1].executable);
    QCOMPARE(layout->sectionAt(0x1004)->name, std::string(".text"));
}

void TestBinary::rejectsOtherFiles()
{
    QVERIFY(!readBinaryLayout(Bytes{}));
    QVERIFY(!readBinaryLayout(Bytes(100, 0x41)));
    Bytes truncated = peFile();
    truncated.resize(0x90); // the PE header is cut off
    QVERIFY(!readBinaryLayout(truncated));
}

void TestBinary::disassemblesAroundAnOffset()
{
    if (!canDisassemble(Architecture::X86_64))
        QSKIP("built without Capstone");
    for (const Bytes& data : {peFile(), elfFile()}) {
        const BinaryLayout layout = *readBinaryLayout(data);
        const std::uint64_t code = layout.sections[0].fileOffset;
        // "mov eax, 1" starts 4 bytes in; asking for a byte inside it finds it.
        const std::vector<Instruction> instructions = disassembleAround(data, layout, code + 5, 2, 1);
        QCOMPARE(instructions.size(), 4u);
        QCOMPARE(instructions[0].text, std::string("push rbp"));
        QCOMPARE(instructions[1].text, std::string("mov rbp, rsp"));
        QCOMPARE(instructions[2].text, std::string("mov eax, 1"));
        QCOMPARE(instructions[2].offset, code + 4);
        QCOMPARE(instructions[2].address, layout.sections[0].address + 4);
        QCOMPARE(instructions[2].bytes.size(), 5u);
        QCOMPARE(instructions[3].text, std::string("pop rbp"));
    }
    // Not in code: nothing.
    const Bytes data = peFile();
    QVERIFY(disassembleAround(data, *readBinaryLayout(data), 0x10).empty());
}

QTEST_APPLESS_MAIN(TestBinary)
#include "tst_binary.moc"
