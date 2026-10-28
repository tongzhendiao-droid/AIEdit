#include "core/Epl.h"

#include "core/EplLibraries.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace aiedit::core::epl {

namespace {

constexpr std::uint32_t kSectionMagic = 0x15117319;
constexpr std::int32_t kSystemInfoSection = 0x02007319;
constexpr std::int32_t kCodeSection = 0x03007319;
constexpr std::int32_t kResourceSection = 0x04007319;
constexpr std::int32_t kLosableSection = 0x05007319;
constexpr std::int32_t kEndOfFileSection = 0x07007319;
constexpr std::int32_t kClassPublicitySection = 0x0B007319;

// Kinds of ids, in their top byte.
constexpr std::uint32_t kTypeMask = 0xFF000000;
constexpr std::uint32_t kTypeMethod = 0x04000000;
constexpr std::uint32_t kTypeGlobal = 0x05000000;
constexpr std::uint32_t kTypeFormSelf = 0x06000000;
constexpr std::uint32_t kTypeStaticClass = 0x09000000;
constexpr std::uint32_t kTypeDll = 0x0A000000;
constexpr std::uint32_t kTypeClassMember = 0x15000000;
constexpr std::uint32_t kTypeFormControl = 0x16000000;
constexpr std::uint32_t kTypeConstant = 0x18000000;
constexpr std::uint32_t kTypeFormClass = 0x19000000;
constexpr std::uint32_t kTypeLocal = 0x25000000;
constexpr std::uint32_t kTypeFormMenu = 0x26000000;
constexpr std::uint32_t kTypeImage = 0x28000000;
constexpr std::uint32_t kTypeStructMember = 0x35000000;
constexpr std::uint32_t kTypeSound = 0x38000000;
constexpr std::uint32_t kTypeStruct = 0x41000000;
constexpr std::uint32_t kTypeDllParameter = 0x45000000;
constexpr std::uint32_t kTypeClass = 0x49000000;
constexpr std::uint32_t kTypeForm = 0x52000000;
constexpr std::int32_t kThisCall = 0x0500FFFE;

std::uint32_t typeOf(std::int32_t id)
{
    return static_cast<std::uint32_t>(id) & kTypeMask;
}

bool isLibDataType(std::int32_t id)
{
    return typeOf(id) == 0 && id != 0;
}

// Little-endian reading that never goes past the end: once it would, everything reads as zero
// and failed() says so.
class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> data) : m_data(data) {}

    bool failed() const { return m_failed; }
    bool atEnd() const { return m_pos >= m_data.size(); }
    std::size_t pos() const { return m_pos; }
    std::size_t remaining() const { return m_failed ? 0 : m_data.size() - m_pos; }
    void seek(std::size_t pos)
    {
        if (pos > m_data.size())
            m_failed = true;
        else
            m_pos = pos;
    }
    void back() { m_pos = m_pos > 0 ? m_pos - 1 : 0; }

    std::span<const std::uint8_t> take(std::size_t n)
    {
        if (m_failed || n > m_data.size() - m_pos) {
            m_failed = true;
            m_pos = m_data.size();
            return {};
        }
        const auto out = m_data.subspan(m_pos, n);
        m_pos += n;
        return out;
    }
    void skip(std::size_t n) { take(n); }

    std::uint8_t u8()
    {
        const auto b = take(1);
        return b.empty() ? 0 : b[0];
    }
    std::int16_t i16()
    {
        const auto b = take(2);
        return b.empty() ? 0 : static_cast<std::int16_t>(b[0] | (b[1] << 8));
    }
    std::int32_t i32()
    {
        const auto b = take(4);
        if (b.empty())
            return 0;
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
                                         (static_cast<std::uint32_t>(b[2]) << 16) |
                                         (static_cast<std::uint32_t>(b[3]) << 24));
    }
    double f64()
    {
        const auto b = take(8);
        double value = 0;
        if (!b.empty())
            std::memcpy(&value, b.data(), sizeof value); // little-endian, as every platform here
        return value;
    }
    // A count that cannot be: negative, or more than the bytes left could hold.
    std::int32_t count(std::size_t bytesEach = 1)
    {
        const std::int32_t n = i32();
        if (n < 0 || static_cast<std::size_t>(n) * bytesEach > remaining()) {
            m_failed = true;
            return 0;
        }
        return n;
    }

    Bytes bytes(std::size_t n)
    {
        const auto b = take(n);
        return Bytes(reinterpret_cast<const char*>(b.data()), b.size());
    }
    // An int32 length, then that many bytes; text stops at a NUL.
    Bytes lengthPrefixed()
    {
        Bytes s = bytes(static_cast<std::size_t>(count()));
        if (const auto nul = s.find('\0'); nul != Bytes::npos)
            s.resize(nul);
        return s;
    }
    Bytes cString()
    {
        Bytes s;
        while (!m_failed && !atEnd()) {
            const char c = static_cast<char>(m_data[m_pos++]);
            if (c == '\0')
                return s;
            s += c;
        }
        m_failed = true;
        return s;
    }
    // The length includes a NUL at the end; 0 means none at all.
    Bytes bstr(bool* present = nullptr)
    {
        const std::int32_t length = count();
        if (present)
            *present = length > 0;
        if (length <= 0)
            return {};
        Bytes s = bytes(static_cast<std::size_t>(length - 1));
        skip(1);
        if (const auto nul = s.find('\0'); nul != Bytes::npos)
            s.resize(nul);
        return s;
    }
    std::vector<std::int32_t> int32s(std::size_t n)
    {
        std::vector<std::int32_t> out;
        if (n * 4 > remaining()) {
            m_failed = true;
            return out;
        }
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            out.push_back(i32());
        return out;
    }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
    bool m_failed = false;
};

// Blocks: an int32 size of the header (8 bytes an item), the ids, then the memory addresses,
// then each item.
template <typename Item, typename ReadItem>
std::vector<Item> blocksWithAddresses(Reader& r, ReadItem readItem)
{
    const std::int32_t headerSize = r.count();
    const std::size_t n = static_cast<std::size_t>(headerSize) / 8;
    const std::vector<std::int32_t> ids = r.int32s(n);
    r.int32s(n); // where the IDE kept them in memory
    std::vector<Item> items;
    for (std::size_t i = 0; i < ids.size() && !r.failed(); ++i) {
        Item item;
        item.id = ids[i];
        readItem(r, item);
        items.push_back(std::move(item));
    }
    return items;
}

// Blocks: a count, the size of what follows, the ids, the offsets, then items of given lengths.
template <typename Item, typename ReadItem>
std::vector<Item> blocksWithOffsets(Reader& r, ReadItem readItem)
{
    const std::int32_t n = r.count(8);
    const std::int32_t size = r.count();
    const std::size_t end = r.pos() + static_cast<std::size_t>(size);
    const std::vector<std::int32_t> ids = r.int32s(static_cast<std::size_t>(n));
    const std::vector<std::int32_t> offsets = r.int32s(static_cast<std::size_t>(n));
    const std::size_t start = r.pos();
    std::vector<Item> items;
    for (std::size_t i = 0; i < ids.size() && !r.failed(); ++i) {
        if (offsets[i] < 0)
            break;
        r.seek(start + static_cast<std::size_t>(offsets[i]));
        const std::int32_t length = r.count();
        Item item;
        item.id = ids[i];
        readItem(r, item, length);
        items.push_back(std::move(item));
    }
    r.seek(end);
    return items;
}

std::vector<Variable> readVariables(Reader& r)
{
    return blocksWithOffsets<Variable>(r, [](Reader& in, Variable& v, std::int32_t) {
        v.dataType = in.i32();
        v.flags = in.i16();
        v.bounds = in.int32s(in.u8());
        v.name = in.cString();
        v.comment = in.cString();
    });
}

std::vector<Library> readLibraries(Reader& r)
{
    // An MFC-style count: 16 bits, or 0xFFFF and then 32.
    std::int32_t n = static_cast<std::uint16_t>(r.i16());
    if (n == 0xFFFF)
        n = r.count();
    std::vector<Library> libraries;
    for (std::int32_t i = 0; i < n && !r.failed(); ++i) {
        const Bytes line = r.lengthPrefixed();
        // "file\rguid\rmajor\rminor\rname"
        std::vector<Bytes> parts;
        std::size_t from = 0;
        for (;;) {
            const std::size_t cr = line.find('\r', from);
            parts.push_back(line.substr(from, cr == Bytes::npos ? Bytes::npos : cr - from));
            if (cr == Bytes::npos)
                break;
            from = cr + 1;
        }
        parts.resize(std::max<std::size_t>(parts.size(), 5));
        Library library;
        library.fileName = parts[0];
        library.guid = parts[1];
        std::from_chars(parts[2].data(), parts[2].data() + parts[2].size(), library.major);
        std::from_chars(parts[3].data(), parts[3].data() + parts[3].size(), library.minor);
        library.name = parts[4];
        libraries.push_back(std::move(library));
    }
    return libraries;
}

void readCodeSection(Reader& r, Project& p)
{
    r.i32(); // the highest id handed out
    r.i32();
    r.skip(static_cast<std::size_t>(r.count())); // the commands each library must have, at least
    r.skip(static_cast<std::size_t>(r.count())); // data types
    r.skip(static_cast<std::size_t>(r.count())); // constants
    p.libraries = readLibraries(r);
    const std::int32_t flags = r.i32();
    p.mainMethod = r.i32();
    if (flags & 1)
        r.skip(16);
    r.skip(static_cast<std::size_t>(r.count())); // the icon
    r.lengthPrefixed();                          // command line arguments for debugging

    p.classes = blocksWithAddresses<Class>(r, [](Reader& in, Class& c) {
        c.form = in.i32();
        c.baseClass = in.i32();
        c.name = in.lengthPrefixed();
        c.comment = in.lengthPrefixed();
        c.methods = in.int32s(static_cast<std::size_t>(in.count()) / 4);
        c.variables = readVariables(in);
    });
    p.methods = blocksWithAddresses<Method>(r, [](Reader& in, Method& m) {
        m.classId = in.i32();
        m.flags = in.i32();
        m.returnType = in.i32();
        m.name = in.lengthPrefixed();
        m.comment = in.lengthPrefixed();
        m.locals = readVariables(in);
        m.parameters = readVariables(in);
        for (int i = 0; i < 5; ++i)
            in.skip(static_cast<std::size_t>(in.count())); // offsets of lines and blocks, references
        m.code = in.bytes(static_cast<std::size_t>(in.count()));
    });
    p.globals = readVariables(r);
    p.structs = blocksWithAddresses<Struct>(r, [](Reader& in, Struct& s) {
        s.flags = in.i32();
        s.name = in.lengthPrefixed();
        s.comment = in.lengthPrefixed();
        s.members = readVariables(in);
    });
    p.dlls = blocksWithAddresses<Dll>(r, [](Reader& in, Dll& d) {
        d.flags = in.i32();
        d.returnType = in.i32();
        d.name = in.lengthPrefixed();
        d.comment = in.lengthPrefixed();
        d.library = in.lengthPrefixed();
        d.entryPoint = in.lengthPrefixed();
        d.parameters = readVariables(in);
    });
}

void readResourceSection(Reader& r, Project& p)
{
    p.forms = blocksWithAddresses<Form>(r, [](Reader& in, Form& f) {
        in.i32();
        f.classId = in.i32();
        f.name = in.lengthPrefixed();
        f.comment = in.lengthPrefixed();
        f.elements = blocksWithOffsets<FormElement>(in, [](Reader& element, FormElement& e, std::int32_t) {
            e.dataType = element.i32();
            element.skip(20);
            e.name = element.cString(); // the rest, a control's or a menu's, is not needed here
        });
    });
    p.constants = blocksWithOffsets<Constant>(r, [](Reader& in, Constant& c, std::int32_t) {
        c.flags = in.i16();
        c.name = in.cString();
        c.comment = in.cString();
        switch (static_cast<std::uint32_t>(c.id) >> 28) {
        case 1:
            switch (in.u8()) {
            case 22:
                c.kind = Constant::Kind::Empty;
                break;
            case 23:
                c.kind = Constant::Kind::Number;
                c.number = in.f64();
                break;
            case 24:
                c.kind = Constant::Kind::Boolean;
                c.number = in.i32() != 0 ? 1 : 0;
                break;
            case 25:
                c.kind = Constant::Kind::Date;
                c.number = in.f64();
                break;
            case 26:
                c.kind = Constant::Kind::Text;
                c.text = in.bstr();
                break;
            default:
                break;
            }
            break;
        case 2:
        case 3:
            c.kind = (static_cast<std::uint32_t>(c.id) >> 28) == 2 ? Constant::Kind::Image : Constant::Kind::Sound;
            c.resourceSize = static_cast<std::size_t>(in.count());
            in.skip(c.resourceSize);
            break;
        default:
            break;
        }
    });
}

} // namespace

Project parse(std::span<const std::uint8_t> data)
{
    Project p;
    Reader r(data);
    const Bytes magic = r.bytes(8);
    if (magic.starts_with("WTLE")) {
        p.status = Project::Status::Encrypted;
        return p;
    }
    if (magic != "CNWTEPRG")
        return p;
    p.status = Project::Status::Ok;
    while (!r.failed() && !r.atEnd()) {
        if (static_cast<std::uint32_t>(r.i32()) != kSectionMagic) {
            p.status = Project::Status::Broken;
            break;
        }
        r.i32(); // checksum
        const std::int32_t key = r.i32();
        r.skip(30 + 2 + 4 + 4 + 4); // name, padding, index, optional, checksum of the data
        const std::int32_t length = r.count();
        r.skip(40);
        const auto body = r.take(static_cast<std::size_t>(length));
        if (r.failed())
            break;
        Reader section(body);
        if (key == kSystemInfoSection) {
            section.i32(); // version of the IDE
            section.i32();
            p.language = section.i32();
            section.i32(); // version of the format
            p.fileType = section.i32();
        } else if (key == kCodeSection) {
            readCodeSection(section, p);
        } else if (key == kResourceSection) {
            readResourceSection(section, p);
        } else if (key == kLosableSection) {
            section.lengthPrefixed(); // the file last compiled to
            const std::int32_t n = section.count(12);
            const std::int32_t size = section.count();
            const std::size_t end = section.pos() + static_cast<std::size_t>(size);
            const std::vector<std::int32_t> ids = section.int32s(static_cast<std::size_t>(n));
            section.int32s(static_cast<std::size_t>(n)); // a checksum of each name
            const std::vector<std::int32_t> offsets = section.int32s(static_cast<std::size_t>(n));
            const std::size_t start = section.pos();
            for (std::size_t i = 0; i < ids.size() && !section.failed(); ++i) {
                section.seek(start + static_cast<std::size_t>(std::max(0, offsets[i])));
                p.removedNames.emplace_back(ids[i], section.cString());
            }
            if (!section.failed())
                section.seek(end);
        } else if (key == kClassPublicitySection) {
            // Which classes are public, kept apart from the classes themselves.
            while (section.remaining() >= 8) {
                const std::int32_t id = section.i32();
                const std::int32_t flags = section.i32();
                for (Class& c : p.classes) {
                    if (c.id == id)
                        c.isPublic = (flags & 1) != 0;
                }
            }
        }
        if (section.failed())
            p.status = Project::Status::Broken;
        if (key == kEndOfFileSection)
            break;
    }
    if (r.failed())
        p.status = Project::Status::Broken;
    return p;
}

// --- Text ---------------------------------------------------------------------------------------

namespace {

std::string formatNumber(double v)
{
    if (!std::isfinite(v))
        return "0";
    if (std::floor(v) == v && std::abs(v) < 1e15)
        return std::to_string(static_cast<long long>(v));
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, v);
    return std::string(buffer, result.ptr);
}

// An OLE date (days since 1899-12-30) as the IDE writes it: 2024年05月01日, with the time if any.
std::string formatDate(double oleDate)
{
    if (!std::isfinite(oleDate) || std::abs(oleDate) > 3e6)
        return {};
    double whole = std::floor(oleDate);
    auto seconds = static_cast<long long>(std::llround((oleDate - whole) * 86400.0));
    if (seconds >= 86400) {
        seconds -= 86400;
        whole += 1;
    }
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    long long z = static_cast<long long>(whole) - 25569 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long year = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long day = doy - (153 * mp + 2) / 5 + 1;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    if (month <= 2)
        ++year;
    auto pad = [](long long v, std::size_t width) {
        std::string digits = std::to_string(v < 0 ? -v : v);
        if (digits.size() < width)
            digits.insert(0, width - digits.size(), '0');
        return (v < 0 ? "-" : "") + digits;
    };
    std::string text = pad(year, 4) + "年" + pad(month, 2) + "月" + pad(day, 2) + "日";
    if (seconds != 0)
        text += pad(seconds / 3600, 2) + "时" + pad(seconds / 60 % 60, 2) + "分" + pad(seconds % 60, 2) + "秒";
    return text;
}

std::string hex6(std::uint32_t v)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%06X", v & 0xFFFFFF);
    return buffer;
}

// One expression of code, as read.
struct Expr
{
    enum class Kind
    {
        ParamEnd,
        ArrayEnd,
        Default, // a parameter left out
        Number,
        Boolean,
        Date,
        String,
        Constant,     // a user's constant, or a library's (library >= 0)
        EnumConstant, // a library type's member
        MethodPtr,
        Call,
        Variable,
        Member,    // of a user's type or form
        LibMember, // of a library's type
        Index,
        Array,
    };
    Kind kind = Kind::Default;
    double number = 0;
    Bytes text;
    std::int32_t id = 0;
    std::int32_t library = -2;
    std::int32_t type = 0;
    std::int32_t member = 0;
    bool invokeSpecial = false;
    std::vector<Expr> target; // none or one: a call's object, what a member or index is of
    std::vector<Expr> items;  // a call's parameters, an array's items, an index
};

class Writer
{
public:
    Writer(const Project& p, const Decoder& decode) : m_p(p), m_decode(decode)
    {
        for (std::size_t i = 0; i < p.libraries.size(); ++i)
            m_libraryNames.push_back(lowerAscii(p.libraries[i].fileName));
        auto add = [&](std::int32_t id, const Bytes& name) {
            if (!name.empty())
                m_names.try_emplace(id, name);
        };
        // A module's methods and classes may keep their names only in a comment: _-@S<名字>.
        auto addNamed = [&](std::int32_t id, const Bytes& name, const Bytes& comment) {
            if (!name.empty()) {
                add(id, name);
            } else if (comment.size() > 6 && comment.starts_with("_-@") && (comment[3] == 'M' || comment[3] == 'S') &&
                       comment[4] == '<' && comment.back() == '>') {
                add(id, comment.substr(5, comment.size() - 6));
            }
        };
        for (const Method& m : p.methods) {
            addNamed(m.id, m.name, m.comment);
            for (const Variable& v : m.parameters)
                add(v.id, v.name);
            for (const Variable& v : m.locals)
                add(v.id, v.name);
        }
        for (const Dll& d : p.dlls) {
            add(d.id, d.name);
            for (const Variable& v : d.parameters)
                add(v.id, v.name);
        }
        for (const Class& c : p.classes) {
            addNamed(c.id, c.name, c.comment);
            for (const std::int32_t method : c.methods)
                m_classOfMethod.try_emplace(method, c.id);
            for (const Variable& v : c.variables)
                add(v.id, v.name);
        }
        for (const Struct& s : p.structs) {
            add(s.id, s.name);
            for (const Variable& v : s.members)
                add(v.id, v.name);
        }
        for (const Variable& v : p.globals)
            add(v.id, v.name);
        for (const Constant& c : p.constants)
            add(c.id, c.name);
        for (const Form& f : p.forms) {
            add(f.id, f.name);
            for (const FormElement& e : f.elements)
                add(e.id, e.name);
        }
        // Last: a name in use wins over one removed with the same id.
        for (const auto& [id, name] : p.removedNames)
            add(id, name);
    }

    std::string text(std::size_t* unreadable)
    {
        out += ".版本 2\n";
        for (const Library& library : m_p.libraries) {
            if (lowerAscii(library.fileName) != "krnln")
                out += ".支持库 " + m_decode(library.fileName) + "\n";
        }

        std::vector<bool> written(m_p.methods.size(), false);
        for (const Class& c : m_p.classes) {
            out += "\n";
            definition(0, "程序集",
                       {name(c.id), c.baseClass == 0 || c.baseClass == -1 ? std::string() : name(c.baseClass),
                        c.isPublic ? "公开" : "", m_decode(c.comment)});
            for (const Variable& v : c.variables)
                if (!v.name.empty()) // the IDE leaves out variables without a name
                    definition(0, "程序集变量", {name(v.id), typeName(v.dataType), std::string(), bounds(v), m_decode(v.comment)});
            for (const std::int32_t id : c.methods) {
                for (std::size_t i = 0; i < m_p.methods.size(); ++i) {
                    if (m_p.methods[i].id == id && !written[i]) {
                        written[i] = true;
                        out += "\n";
                        method(m_p.methods[i]);
                    }
                }
            }
        }
        bool orphans = false;
        for (std::size_t i = 0; i < m_p.methods.size(); ++i) {
            if (written[i])
                continue;
            if (!orphans)
                out += "\n";
            orphans = true;
            out += "\n";
            method(m_p.methods[i]);
        }

        if (!m_p.globals.empty()) {
            out += "\n";
            for (const Variable& v : m_p.globals)
                if (!v.name.empty()) // the IDE leaves out variables without a name
                    definition(0, "全局变量",
                           {name(v.id), typeName(v.dataType), (v.flags & 0x100) ? "公开" : "", bounds(v), m_decode(v.comment)});
        }
        for (const Struct& s : m_p.structs) {
            out += "\n";
            definition(0, "数据类型", {name(s.id), (s.flags & 1) ? "公开" : "", m_decode(s.comment)});
            for (const Variable& v : s.members)
                if (!v.name.empty()) // the IDE leaves out variables without a name
                    definition(1, "成员", {name(v.id), typeName(v.dataType), (v.flags & 2) ? "传址" : "", bounds(v),
                                       m_decode(v.comment)});
        }
        for (const Dll& d : m_p.dlls) {
            out += "\n";
            definition(0, "DLL命令",
                       {name(d.id), typeName(d.returnType), quoted(m_decode(d.library)), quoted(m_decode(d.entryPoint)),
                        (d.flags & 2) ? "公开" : "", m_decode(d.comment)});
            for (const Variable& v : d.parameters)
                if (!v.name.empty()) // the IDE leaves out variables without a name
                    definition(1, "参数", {name(v.id), typeName(v.dataType), dllParameterFlags(v), m_decode(v.comment)});
        }
        if (!m_p.constants.empty())
            out += "\n";
        for (const Constant& c : m_p.constants)
            constant(c);
        if (unreadable)
            *unreadable = m_unreadable;
        return std::move(out);
    }

private:
    static std::string lowerAscii(std::string s)
    {
        for (char& c : s)
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        return s;
    }

    static std::string quoted(const std::string& s) { return s.empty() ? s : "\"" + s + "\""; }

    std::string name(std::int32_t id) const
    {
        if (const auto it = m_names.find(id); it != m_names.end())
            return m_decode(it->second);
        if (id == m_p.mainMethod && id != 0)
            return "_启动子程序";
        const char* kind = nullptr;
        switch (typeOf(id)) {
        case kTypeMethod:
            kind = "Sub";
            break;
        case kTypeGlobal:
            kind = "Global";
            break;
        case kTypeStaticClass:
            kind = "Mod";
            break;
        case kTypeDll:
            kind = "Dll";
            break;
        case kTypeClassMember:
            kind = "Mem";
            break;
        case kTypeFormControl:
            kind = "Control";
            break;
        case kTypeFormMenu:
            kind = "Menu";
            break;
        case kTypeConstant:
            kind = "Const";
            break;
        case kTypeFormClass:
            kind = "FormCls";
            break;
        case kTypeLocal:
            kind = "Local";
            break;
        case kTypeImage:
            kind = "Img";
            break;
        case kTypeSound:
            kind = "Sound";
            break;
        case kTypeStructMember:
            kind = "StructMem";
            break;
        case kTypeStruct:
            kind = "Struct";
            break;
        case kTypeDllParameter:
            kind = "DllParam";
            break;
        case kTypeClass:
            kind = "Cls";
            break;
        case kTypeForm:
            kind = "Form";
            break;
        case kTypeFormSelf:
            return {};
        default:
            break;
        }
        return kind ? std::string("_") + kind + "_" + hex6(static_cast<std::uint32_t>(id))
                    : "_User_" + hex6(static_cast<std::uint32_t>(id));
    }

    // The library's file name, lower case: what the name tables are kept by.
    std::string library(std::int32_t index) const
    {
        return index >= 0 && static_cast<std::size_t>(index) < m_libraryNames.size()
                   ? m_libraryNames[static_cast<std::size_t>(index)]
                   : std::string("lib") + std::to_string(index);
    }

    std::string typeName(std::int32_t id) const
    {
        switch (static_cast<std::uint32_t>(id)) {
        case 0:
            return {};
        case 0x80000000:
            return "通用型";
        case 0x80000101:
            return "字节型";
        case 0x80000201:
            return "短整数型";
        case 0x80000301:
            return "整数型";
        case 0x80000401:
            return "长整数型";
        case 0x80000501:
            return "小数型";
        case 0x80000601:
            return "双精度小数型";
        case 0x80000002:
            return "逻辑型";
        case 0x80000003:
            return "日期时间型";
        case 0x80000004:
            return "文本型";
        case 0x80000005:
            return "字节集";
        case 0x80000006:
            return "子程序指针";
        case 0x80000008:
            return "条件语句型";
        default:
            break;
        }
        if (isLibDataType(id))
            return libraryTypeName(library((id >> 16) - 1), (id & 0xFFFF) - 1);
        return name(id);
    }

    std::string bounds(const Variable& v) const
    {
        if (v.bounds.empty())
            return {};
        if (v.bounds.size() == 1 && v.bounds[0] == 0)
            return "\"0\"";
        std::string s = "\"";
        for (std::size_t i = 0; i < v.bounds.size(); ++i) {
            if (i > 0)
                s += ",";
            if (v.bounds[i] != 0)
                s += std::to_string(v.bounds[i]);
        }
        return s + "\"";
    }

    static std::string dllParameterFlags(const Variable& v)
    {
        std::string s;
        if (v.flags & 2)
            s += "传址";
        if (v.flags & 8)
            s += s.empty() ? "数组" : " 数组";
        return s;
    }

    static std::string parameterFlags(const Variable& v)
    {
        std::string s;
        for (const auto& [flag, word] : {std::pair{2, "参考"}, std::pair{4, "可空"}, std::pair{8, "数组"}}) {
            if (v.flags & flag)
                s += (s.empty() ? "" : " ") + std::string(word);
        }
        return s;
    }

    // ".类型 a, b, c" without the empty items at the end.
    void definition(int indent, const char* type, std::vector<std::string> items)
    {
        while (!items.empty() && items.back().empty())
            items.pop_back();
        out.append(static_cast<std::size_t>(indent) * 4, ' ');
        out += ".";
        out += type;
        for (std::size_t i = 0; i < items.size(); ++i)
            out += (i == 0 ? " " : ", ") + items[i];
        out += "\n";
    }

    void constant(const Constant& c)
    {
        std::string value;
        switch (c.kind) {
        case Constant::Kind::Empty:
            break;
        case Constant::Kind::Number:
            value = "\"" + formatNumber(c.number) + "\"";
            break;
        case Constant::Kind::Boolean:
            value = c.number != 0 ? "\"真\"" : "\"假\"";
            break;
        case Constant::Kind::Date:
            value = "\"[" + formatDate(c.number) + "]\"";
            break;
        case Constant::Kind::Text:
            value = "\"“" + m_decode(c.text) + "”\"";
            break;
        case Constant::Kind::Image:
        case Constant::Kind::Sound:
            definition(0, c.kind == Constant::Kind::Image ? "图片" : "声音",
                       {name(c.id), "\"<" + std::to_string(c.resourceSize) + " 字节>\"", (c.flags & 2) ? "公开" : "",
                        m_decode(c.comment)});
            return;
        }
        definition(0, "常量", {name(c.id), value, (c.flags & 2) ? "公开" : "", m_decode(c.comment)});
    }

    void method(const Method& m)
    {
        definition(0, "子程序", {name(m.id), typeName(m.returnType), (m.flags & 8) ? "公开" : "", m_decode(m.comment)});
        for (const Variable& v : m.parameters)
            if (!v.name.empty()) // the IDE leaves out variables without a name
                definition(0, "参数", {name(v.id), typeName(v.dataType), parameterFlags(v), m_decode(v.comment)});
        for (const Variable& v : m.locals)
            if (!v.name.empty()) // the IDE leaves out variables without a name
                definition(0, "局部变量", {name(v.id), typeName(v.dataType), (v.flags & 1) ? "静态" : "", bounds(v),
                                       m_decode(v.comment)});
        out += "\n";
        Reader r(std::span(reinterpret_cast<const std::uint8_t*>(m.code.data()), m.code.size()));
        m_codeFailed = false;
        const std::size_t before = out.size();
        block(r, 0);
        if (m_codeFailed || r.failed()) {
            ++m_unreadable;
            out += "' ……（以下代码读不出来）\n";
        } else if (out.size() == before) {
            out += "\n"; // an empty method still has its one empty line
        }
    }

    // --- Statements ---

    // Statement kinds, the first byte of each.
    enum : std::uint8_t
    {
        Else = 0x50,
        IfEnd = 0x51,
        IfTrueEnd = 0x52,
        CaseEnd = 0x53,
        SwitchEnd = 0x54,
        LoopBodyEnd = 0x55,
        CallStatement = 0x6A,
        If = 0x6B,
        IfTrue = 0x6C,
        Switch = 0x6D,
        Case = 0x6E,
        Default = 0x6F,
        LoopStart = 0x70,
        LoopEnd = 0x71,
    };

    static bool isStatement(std::uint8_t b)
    {
        return (b >= Else && b <= LoopBodyEnd) || (b >= CallStatement && b <= LoopEnd);
    }

    struct Line
    {
        Expr call;
        Bytes unexamined; // code the IDE could not make sense of, kept as typed
        Bytes comment;
        bool hasComment = false; // the IDE writes an empty comment too: "  ' "
        bool mask = false;       // commented out
    };

    // The statements up to the end of a block, which is left for the caller to read.
    void block(Reader& r, int indent)
    {
        const std::size_t before = out.size();
        while (!m_codeFailed && !r.failed()) {
            std::uint8_t kind = 0;
            do {
                if (r.atEnd())
                    return finishBlock(before);
                kind = r.u8();
            } while (!isStatement(kind));
            switch (kind) {
            case Else:
            case IfEnd:
            case IfTrueEnd:
            case CaseEnd:
            case SwitchEnd:
            case Default:
            case LoopEnd:
                r.back();
                return finishBlock(before);
            case LoopBodyEnd:
                continue;
            case Switch:
                switchStatement(r, indent);
                continue;
            default:
                break;
            }
            Line line = callLine(r);
            if (m_codeFailed)
                return;
            switch (kind) {
            case LoopStart:
                loop(r, indent, line);
                break;
            case IfTrue:
                head(indent, line.mask,
                     line.unexamined.empty() ? "如果真 (" + param(line.call, 0) + ")" : m_decode(line.unexamined));
                endLine(line);
                block(r, indent + 1);
                expect(r, IfTrueEnd);
                r.u8();
                startLine(indent, false);
                out += ".如果真结束\n";
                break;
            case If:
                head(indent, line.mask,
                     line.unexamined.empty() ? "如果 (" + param(line.call, 0) + ")" : m_decode(line.unexamined));
                endLine(line);
                block(r, indent + 1);
                expect(r, Else);
                startLine(indent, false);
                out += ".否则\n";
                block(r, indent + 1);
                expect(r, IfEnd);
                r.u8();
                startLine(indent, false);
                out += ".如果结束\n";
                break;
            case CallStatement:
                if (!line.unexamined.empty()) {
                    startLine(indent, line.mask);
                    out += m_decode(line.unexamined) + "\n";
                } else if (line.call.library == -1) {
                    // An empty line, or only a comment.
                    out.append(static_cast<std::size_t>(indent) * 4, ' ');
                    if (line.hasComment)
                        out += "' " + m_decode(line.comment);
                    out += "\n";
                } else {
                    startLine(indent, line.mask);
                    out += expression(line.call);
                    endLine(line);
                }
                break;
            default:
                m_codeFailed = true;
                return;
            }
        }
    }

    void finishBlock(std::size_t before)
    {
        if (out.size() == before)
            out += "\n"; // an empty block shows as an empty line
    }

    void expect(Reader& r, std::uint8_t kind)
    {
        if (r.u8() != kind)
            m_codeFailed = true;
    }

    void startLine(int indent, bool mask)
    {
        out.append(static_cast<std::size_t>(indent) * 4, ' ');
        if (mask)
            out += "' ";
    }

    void endLine(const Line& line)
    {
        if (line.hasComment)
            out += "  ' " + m_decode(line.comment);
        out += "\n";
    }

    // The first line of a block: commented out, the mark goes after the dot (".' 如果 (...)").
    void head(int indent, bool mask, const std::string& text)
    {
        out.append(static_cast<std::size_t>(indent) * 4, ' ');
        out += mask ? ".' " + text : "." + text;
    }

    void switchStatement(Reader& r, int indent)
    {
        if (r.u8() != Case) {
            m_codeFailed = true;
            return;
        }
        bool first = true;
        std::uint8_t next = Case;
        while (next == Case && !m_codeFailed) {
            const Line line = callLine(r);
            if (m_codeFailed)
                return;
            if (line.unexamined.empty()) {
                head(indent, line.mask, (first ? "判断开始 (" : "判断 (") + param(line.call, 0) + ")");
            } else {
                std::string code = m_decode(line.unexamined);
                if (first && code.starts_with("判断"))
                    code = "判断开始" + code.substr(std::strlen("判断"));
                head(indent, line.mask, code);
            }
            endLine(line);
            block(r, indent + 1);
            expect(r, CaseEnd);
            next = r.u8();
            first = false;
        }
        if (next != Default) {
            m_codeFailed = true;
            return;
        }
        startLine(indent, false);
        out += ".默认\n";
        block(r, indent + 1);
        expect(r, SwitchEnd);
        r.u8();
        startLine(indent, false);
        out += ".判断结束\n";
    }

    void loop(Reader& r, int indent, const Line& start)
    {
        if (start.call.library != 0) {
            m_codeFailed = true;
            return;
        }
        const std::int32_t command = start.call.id;
        std::string first;
        std::string tail;
        switch (command) {
        case 3:
            first = "判断循环首 (" + param(start.call, 0) + ")";
            tail = "判断循环尾 ()";
            break;
        case 5:
            first = "循环判断首 ()";
            break;
        case 7:
            first = "计次循环首 (" + param(start.call, 0) + ", " + param(start.call, 1) + ")";
            tail = "计次循环尾 ()";
            break;
        case 9:
            first = "变量循环首 (" + param(start.call, 0) + ", " + param(start.call, 1) + ", " + param(start.call, 2) +
                   ", " + param(start.call, 3) + ")";
            tail = "变量循环尾 ()";
            break;
        default:
            m_codeFailed = true;
            return;
        }
        head(indent, start.mask, !start.unexamined.empty() && command != 5 ? m_decode(start.unexamined) : first);
        endLine(start);
        block(r, indent + 1);
        expect(r, LoopEnd);
        const Line end = callLine(r);
        if (m_codeFailed)
            return;
        if (command == 5)
            tail = end.unexamined.empty() ? "循环判断尾 (" + param(end.call, 0) + ")" : m_decode(end.unexamined);
        head(indent, end.mask, tail);
        endLine(end);
    }

    // --- Expressions ---

    Line callLine(Reader& r)
    {
        Line line;
        line.call = call(r, &line.unexamined, &line.comment, &line.mask, &line.hasComment);
        return line;
    }

    Expr call(Reader& r, Bytes* unexamined = nullptr, Bytes* comment = nullptr, bool* mask = nullptr,
              bool* hasComment = nullptr)
    {
        Expr e;
        e.kind = Expr::Kind::Call;
        e.id = r.i32();
        e.library = r.i16();
        const std::int16_t flags = r.i16();
        const Bytes u = r.bstr();
        bool present = false;
        const Bytes c = r.bstr(&present);
        if (hasComment)
            *hasComment = present;
        if (unexamined)
            *unexamined = u;
        if (comment)
            *comment = c;
        if (mask)
            *mask = (flags & 0x20) != 0;
        e.invokeSpecial = (flags & 0x10) != 0;
        if (r.atEnd())
            return e;
        const std::uint8_t next = r.u8();
        if (next == 0x36) {
            e.items = parameters(r);
        } else if (next == 0x38) {
            r.back();
            e.target.push_back(expression(r, true));
            e.items = parameters(r);
        } else {
            m_codeFailed = true;
        }
        return e;
    }

    std::vector<Expr> parameters(Reader& r)
    {
        std::vector<Expr> list;
        while (!m_codeFailed && !r.failed()) {
            Expr e = expression(r, true);
            if (e.kind == Expr::Kind::ParamEnd)
                break;
            list.push_back(std::move(e));
        }
        return list;
    }

    Expr expression(Reader& r, bool members, int depth = 0)
    {
        Expr e;
        if (depth > 200) {
            m_codeFailed = true;
            return e;
        }
        for (;;) {
            if (r.failed() || m_codeFailed) {
                m_codeFailed = true;
                return e;
            }
            const std::uint8_t kind = r.u8();
            switch (kind) {
            case 0x01:
                e.kind = Expr::Kind::ParamEnd;
                return e;
            case 0x16:
                e.kind = Expr::Kind::Default;
                return e;
            case 0x17:
                e.kind = Expr::Kind::Number;
                e.number = r.f64();
                return e;
            case 0x18:
                e.kind = Expr::Kind::Boolean;
                e.number = r.i16() != 0 ? 1 : 0;
                return e;
            case 0x19:
                e.kind = Expr::Kind::Date;
                e.number = r.f64();
                return e;
            case 0x1A:
                e.kind = Expr::Kind::String;
                e.text = r.bstr();
                return e;
            case 0x1B:
                e.kind = Expr::Kind::Constant;
                e.id = r.i32();
                return e;
            case 0x1C:
                e.kind = Expr::Kind::Constant;
                e.library = r.i16() - 1;
                e.id = r.i16() - 1;
                return e;
            case 0x1D:
            case 0x37:
                continue;
            case 0x1E:
                e.kind = Expr::Kind::MethodPtr;
                e.id = r.i32();
                return e;
            case 0x21:
                e = call(r);
                break;
            case 0x23:
                e.kind = Expr::Kind::EnumConstant;
                e.library = r.i16() - 1;
                e.type = r.i16() - 1;
                e.member = r.i32() - 1;
                return e;
            case 0x1F:
                e.kind = Expr::Kind::Array;
                while (!m_codeFailed && !r.failed()) {
                    Expr item = expression(r, true, depth + 1);
                    if (item.kind == Expr::Kind::ArrayEnd)
                        break;
                    e.items.push_back(std::move(item));
                }
                return e;
            case 0x20:
                e.kind = Expr::Kind::ArrayEnd;
                return e;
            case 0x38: {
                const std::int32_t variable = r.i32();
                if (variable == kThisCall) {
                    r.u8(); // 0x3A
                    return expression(r, true, depth + 1);
                }
                e.kind = Expr::Kind::Variable;
                e.id = variable;
                members = true;
                break;
            }
            case 0x3B:
                e.kind = Expr::Kind::Number;
                e.number = r.i32();
                return e;
            default:
                m_codeFailed = true;
                return e;
            }
            break;
        }
        // What follows a variable or a call: .member and [index], any number of them.
        while (members && !r.atEnd() && !m_codeFailed) {
            const std::uint8_t next = r.u8();
            if (next == 0x39) {
                const std::int32_t member = r.i32();
                const std::int32_t structId = r.i32();
                Expr access;
                access.target.push_back(std::move(e));
                if (isLibDataType(structId)) {
                    access.kind = Expr::Kind::LibMember;
                    access.library = (structId >> 16) - 1;
                    access.type = (structId & 0xFFFF) - 1;
                    access.member = member - 1;
                } else {
                    access.kind = Expr::Kind::Member;
                    access.type = structId;
                    access.member = member;
                }
                e = std::move(access);
            } else if (next == 0x3A) {
                Expr index;
                index.kind = Expr::Kind::Index;
                index.target.push_back(std::move(e));
                index.items.push_back(expression(r, false, depth + 1));
                e = std::move(index);
            } else {
                if (next != 0x37)
                    r.back();
                break;
            }
        }
        return e;
    }

    std::string param(const Expr& call, std::size_t i) const
    {
        return i < call.items.size() ? expression(call.items[i]) : std::string();
    }

    struct Operator
    {
        const char* text;
        int precedence;
        bool unary;
    };
    static const Operator* operatorOf(const Expr& e)
    {
        if (e.kind != Expr::Kind::Call || e.library != 0 || !e.target.empty())
            return nullptr;
        static const std::unordered_map<std::int32_t, Operator> operators = {
            {15, {"×", 2, false}}, {16, {"÷", 2, false}}, {17, {"\\", 3, false}}, {18, {"%", 4, false}},
            {19, {"＋", 5, false}}, {20, {"－", 5, false}}, {21, {"－", 1, true}},  {38, {"＝", 6, false}},
            {39, {"≠", 6, false}},  {40, {"＜", 6, false}}, {41, {"＞", 6, false}}, {42, {"≤", 6, false}},
            {43, {"≥", 6, false}},  {44, {"≈", 6, false}},  {45, {"且", 7, false}}, {46, {"或", 8, false}},
            {52, {"＝", 9, false}},
        };
        const auto it = operators.find(e.id);
        return it == operators.end() ? nullptr : &it->second;
    }

    std::string expression(const Expr& e, int lowest = 1000) const
    {
        switch (e.kind) {
        case Expr::Kind::ParamEnd:
        case Expr::Kind::ArrayEnd:
        case Expr::Kind::Default:
            return {};
        case Expr::Kind::Number:
            return formatNumber(e.number);
        case Expr::Kind::Boolean:
            return e.number != 0 ? "真" : "假";
        case Expr::Kind::Date:
            return "[" + formatDate(e.number) + "]";
        case Expr::Kind::String:
            return "“" + m_decode(e.text) + "”";
        case Expr::Kind::Constant:
            return "#" + (e.library == -2 || e.library < 0 ? name(e.id) : libraryConstantName(library(e.library), e.id));
        case Expr::Kind::EnumConstant:
            return "#" + libraryTypeName(library(e.library), e.type) + "." +
                   libraryMemberName(library(e.library), e.type, e.member);
        case Expr::Kind::MethodPtr:
            return "&" + name(e.id);
        case Expr::Kind::Variable:
            return name(e.id);
        case Expr::Kind::Member: {
            std::string s;
            const Expr& target = e.target.front();
            if (!(target.kind == Expr::Kind::Variable && typeOf(target.id) == kTypeFormSelf))
                s = expression(target) + ".";
            // A form's own property (as 窗口), or a control on it.
            if (typeOf(e.type) == kTypeForm && (static_cast<std::uint32_t>(e.member) & kTypeMask) == 0)
                return s + libraryMemberName("krnln", 0, e.member - 1);
            return s + name(e.member);
        }
        case Expr::Kind::LibMember: {
            std::string s;
            const Expr& target = e.target.front();
            if (!(target.kind == Expr::Kind::Variable && typeOf(target.id) == kTypeFormSelf))
                s = expression(target) + ".";
            return s + libraryMemberName(library(e.library), e.type, e.member);
        }
        case Expr::Kind::Index:
            return expression(e.target.front()) + " [" + (e.items.empty() ? "" : expression(e.items.front())) + "]";
        case Expr::Kind::Array: {
            std::string s = "{ ";
            for (std::size_t i = 0; i < e.items.size(); ++i)
                s += (i == 0 ? "" : ", ") + expression(e.items[i]);
            return s + (e.items.empty() ? "}" : " }");
        }
        case Expr::Kind::Call:
            break;
        }

        if (const Operator* op = operatorOf(e)) {
            std::string s;
            if (op->unary) {
                s = std::string(op->text) + (e.items.empty() ? "" : expression(e.items[0], op->precedence));
            } else if (e.items.empty()) {
                s = std::string(" ") + op->text + " ";
            } else {
                s = expression(e.items[0], op->precedence) + " " + op->text + " ";
                for (std::size_t i = 1; i < e.items.size(); ++i)
                    s += (i > 1 ? std::string(" ") + op->text + " " : "") + expression(e.items[i], op->precedence - 1);
            }
            return op->precedence > lowest ? "(" + s + ")" : s;
        }

        std::string s;
        if (!e.target.empty())
            s = expression(e.target.front()) + ".";
        if (e.invokeSpecial && e.library == -2) {
            // A call of the base class's method: 基类.方法 ().
            if (const auto it = m_classOfMethod.find(e.id); it != m_classOfMethod.end() && typeOf(it->second) == kTypeClass)
                s += name(it->second) + ".";
        }
        s += e.library == -2 || e.library == -3 ? name(e.id) : libraryCommandName(library(e.library), e.id);
        s += " (";
        for (std::size_t i = 0; i < e.items.size(); ++i)
            s += (i == 0 ? "" : ", ") + expression(e.items[i]);
        return s + ")";
    }

    const Project& m_p;
    const Decoder& m_decode;
    std::vector<std::string> m_libraryNames;
    std::unordered_map<std::int32_t, Bytes> m_names;
    std::unordered_map<std::int32_t, std::int32_t> m_classOfMethod;
    std::string out;
    bool m_codeFailed = false;
    std::size_t m_unreadable = 0;
};

} // namespace

std::string toText(const Project& project, const Decoder& decode, std::size_t* unreadable)
{
    return Writer(project, decode).text(unreadable);
}

} // namespace aiedit::core::epl
