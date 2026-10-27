#include "core/Pdf.h"

#include "core/Inflate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aiedit::core {

namespace {

using Bytes = std::vector<std::uint8_t>;

// Each stream decoded; more is left out.
constexpr std::size_t kMaxStream = 256u * 1024 * 1024;
constexpr int kMaxFormDepth = 8;

bool isWhite(std::uint8_t c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0';
}

// Whether `text` has `word` at `pos`; false past its end.
bool hasAt(std::string_view text, std::size_t pos, std::string_view word)
{
    return pos <= text.size() && text.substr(pos).starts_with(word);
}

bool isDelimiter(std::uint8_t c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
           c == '%';
}

int hexValue(std::uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// --- Objects ------------------------------------------------------------------------------------

struct Object
{
    enum class Kind : std::uint8_t
    {
        Null,
        Boolean,
        Number,
        String,
        Name,
        Array,
        Dictionary,
        Reference,
        Keyword, // an operator in a content stream, or a word such as "stream" or "endobj"
        End,     // nothing more to read
    };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    int objectNumber = 0; // of a reference
    std::string text;     // a string's bytes, a name without its slash, a keyword
    std::vector<Object> items;     // an array's, or a dictionary's values
    std::vector<std::string> keys; // a dictionary's keys

    bool is(Kind k) const { return kind == k; }
    bool isName(std::string_view name) const { return kind == Kind::Name && text == name; }
    const Object* get(std::string_view key) const
    {
        if (kind != Kind::Dictionary)
            return nullptr;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key)
                return &items[i];
        }
        return nullptr;
    }
};

class Lexer
{
public:
    explicit Lexer(std::span<const std::uint8_t> data, std::size_t pos = 0) : m_data(data), m_pos(pos) {}

    std::size_t pos() const { return m_pos; }
    void setPos(std::size_t pos) { m_pos = pos; }
    bool atEnd() { return skipWhite(), m_pos >= m_data.size(); }

    // The next object; in a file, "12 0 R" is read as one reference.
    Object next(int depth = 0)
    {
        skipWhite();
        Object o;
        if (m_pos >= m_data.size()) {
            o.kind = Object::Kind::End;
            return o;
        }
        const std::uint8_t c = m_data[m_pos];
        if (c == '/') {
            ++m_pos;
            o.kind = Object::Kind::Name;
            while (m_pos < m_data.size() && !isWhite(m_data[m_pos]) && !isDelimiter(m_data[m_pos])) {
                if (m_data[m_pos] == '#' && m_pos + 2 < m_data.size() && hexValue(m_data[m_pos + 1]) >= 0 &&
                    hexValue(m_data[m_pos + 2]) >= 0) {
                    o.text += static_cast<char>(hexValue(m_data[m_pos + 1]) * 16 + hexValue(m_data[m_pos + 2]));
                    m_pos += 3;
                } else {
                    o.text += static_cast<char>(m_data[m_pos++]);
                }
            }
            return o;
        }
        if (c == '(') {
            ++m_pos;
            o.kind = Object::Kind::String;
            o.text = literalString();
            return o;
        }
        if (c == '<' && m_pos + 1 < m_data.size() && m_data[m_pos + 1] == '<') {
            m_pos += 2;
            o.kind = Object::Kind::Dictionary;
            while (depth < 64) {
                skipWhite();
                if (m_pos + 1 < m_data.size() && m_data[m_pos] == '>' && m_data[m_pos + 1] == '>') {
                    m_pos += 2;
                    break;
                }
                Object key = next(depth + 1);
                if (key.is(Object::Kind::End))
                    break;
                if (!key.is(Object::Kind::Name))
                    continue; // junk; carry on with what follows
                Object value = next(depth + 1);
                if (value.is(Object::Kind::End))
                    break;
                o.keys.push_back(std::move(key.text));
                o.items.push_back(std::move(value));
            }
            return o;
        }
        if (c == '<') {
            ++m_pos;
            o.kind = Object::Kind::String;
            int high = -1;
            while (m_pos < m_data.size() && m_data[m_pos] != '>') {
                const int v = hexValue(m_data[m_pos++]);
                if (v < 0)
                    continue;
                if (high < 0) {
                    high = v;
                } else {
                    o.text += static_cast<char>(high * 16 + v);
                    high = -1;
                }
            }
            if (high >= 0)
                o.text += static_cast<char>(high * 16);
            if (m_pos < m_data.size())
                ++m_pos; // the '>'
            return o;
        }
        if (c == '[') {
            ++m_pos;
            o.kind = Object::Kind::Array;
            while (depth < 64) {
                skipWhite();
                if (m_pos < m_data.size() && m_data[m_pos] == ']') {
                    ++m_pos;
                    break;
                }
                Object item = next(depth + 1);
                if (item.is(Object::Kind::End))
                    break;
                o.items.push_back(std::move(item));
            }
            return o;
        }
        if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
            ++m_pos; // stray; skipped
            return next(depth);
        }
        if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
            const std::size_t start = m_pos;
            o.kind = Object::Kind::Number;
            o.number = number();
            // "12 0 R": a reference.
            if (o.number >= 0 && std::floor(o.number) == o.number && m_data[start] != '+' && m_data[start] != '-' &&
                m_allowReferences) {
                const std::size_t after = m_pos;
                skipWhite();
                const std::size_t genStart = m_pos;
                while (m_pos < m_data.size() && m_data[m_pos] >= '0' && m_data[m_pos] <= '9')
                    ++m_pos;
                if (m_pos > genStart) {
                    skipWhite();
                    if (m_pos < m_data.size() && m_data[m_pos] == 'R' &&
                        (m_pos + 1 >= m_data.size() || isWhite(m_data[m_pos + 1]) || isDelimiter(m_data[m_pos + 1]))) {
                        ++m_pos;
                        o.kind = Object::Kind::Reference;
                        o.objectNumber = static_cast<int>(std::min(o.number, 1e9));
                        return o;
                    }
                }
                m_pos = after;
            }
            return o;
        }
        o.kind = Object::Kind::Keyword;
        while (m_pos < m_data.size() && !isWhite(m_data[m_pos]) && !isDelimiter(m_data[m_pos]))
            o.text += static_cast<char>(m_data[m_pos++]);
        if (o.text.empty()) {
            ++m_pos; // a lone delimiter such as '%' at the end
            return next(depth);
        }
        if (o.text == "true" || o.text == "false") {
            o.kind = Object::Kind::Boolean;
            o.boolean = o.text == "true";
        } else if (o.text == "null") {
            o.kind = Object::Kind::Null;
        }
        return o;
    }

    // Content streams have no references: "0 0 Td" must not be read as one.
    void setAllowReferences(bool allow) { m_allowReferences = allow; }

    void skipWhite()
    {
        while (m_pos < m_data.size()) {
            const std::uint8_t c = m_data[m_pos];
            if (isWhite(c)) {
                ++m_pos;
            } else if (c == '%') {
                while (m_pos < m_data.size() && m_data[m_pos] != '\n' && m_data[m_pos] != '\r')
                    ++m_pos;
            } else {
                break;
            }
        }
    }

private:
    double number()
    {
        bool negative = false;
        if (m_data[m_pos] == '+' || m_data[m_pos] == '-')
            negative = m_data[m_pos++] == '-';
        double value = 0;
        while (m_pos < m_data.size() && m_data[m_pos] >= '0' && m_data[m_pos] <= '9')
            value = value * 10 + (m_data[m_pos++] - '0');
        if (m_pos < m_data.size() && m_data[m_pos] == '.') {
            ++m_pos;
            double scale = 0.1;
            while (m_pos < m_data.size() && m_data[m_pos] >= '0' && m_data[m_pos] <= '9') {
                value += (m_data[m_pos++] - '0') * scale;
                scale /= 10;
            }
        }
        // "--5" and such: what writers mean is the number.
        while (m_pos < m_data.size() && (m_data[m_pos] == '-' || m_data[m_pos] == '+'))
            ++m_pos;
        return negative ? -value : value;
    }

    std::string literalString()
    {
        std::string s;
        int nesting = 1;
        while (m_pos < m_data.size()) {
            const std::uint8_t c = m_data[m_pos++];
            if (c == '(') {
                ++nesting;
            } else if (c == ')') {
                if (--nesting == 0)
                    break;
            } else if (c == '\\' && m_pos < m_data.size()) {
                const std::uint8_t e = m_data[m_pos++];
                switch (e) {
                case 'n':
                    s += '\n';
                    continue;
                case 'r':
                    s += '\r';
                    continue;
                case 't':
                    s += '\t';
                    continue;
                case 'b':
                    s += '\b';
                    continue;
                case 'f':
                    s += '\f';
                    continue;
                case '\r':
                    if (m_pos < m_data.size() && m_data[m_pos] == '\n')
                        ++m_pos;
                    continue;
                case '\n':
                    continue;
                default:
                    break;
                }
                if (e >= '0' && e <= '7') {
                    int value = e - '0';
                    for (int i = 0; i < 2 && m_pos < m_data.size() && m_data[m_pos] >= '0' && m_data[m_pos] <= '7'; ++i)
                        value = value * 8 + (m_data[m_pos++] - '0');
                    s += static_cast<char>(value & 0xFF);
                } else {
                    s += static_cast<char>(e);
                }
                continue;
            }
            s += static_cast<char>(c);
        }
        return s;
    }

    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
    bool m_allowReferences = true;
};

// --- Filters ------------------------------------------------------------------------------------

bool asciiHex(std::span<const std::uint8_t> in, Bytes& out)
{
    int high = -1;
    for (const std::uint8_t c : in) {
        if (c == '>')
            break;
        const int v = hexValue(c);
        if (v < 0)
            continue;
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<std::uint8_t>(high * 16 + v));
            high = -1;
        }
    }
    if (high >= 0)
        out.push_back(static_cast<std::uint8_t>(high * 16));
    return true;
}

bool ascii85(std::span<const std::uint8_t> in, Bytes& out)
{
    std::uint32_t tuple = 0;
    int count = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::uint8_t c = in[i];
        if (c == '~')
            break;
        if (isWhite(c))
            continue;
        if (c == 'z' && count == 0) {
            out.insert(out.end(), 4, 0);
            continue;
        }
        if (c < '!' || c > 'u')
            return false;
        tuple = tuple * 85 + (c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k)
                out.push_back(static_cast<std::uint8_t>(tuple >> (k * 8)));
            tuple = 0;
            count = 0;
        }
    }
    if (count > 1) {
        for (int k = count; k < 5; ++k)
            tuple = tuple * 85 + 84;
        for (int k = 3; k > 4 - count; --k)
            out.push_back(static_cast<std::uint8_t>(tuple >> (k * 8)));
    }
    return true;
}

bool lzw(std::span<const std::uint8_t> in, int earlyChange, Bytes& out)
{
    std::vector<Bytes> table;
    auto reset = [&] {
        table.assign(258, {});
        for (int i = 0; i < 256; ++i)
            table[i] = {static_cast<std::uint8_t>(i)};
    };
    reset();
    int bits = 9;
    std::uint32_t buffer = 0;
    int bufferBits = 0;
    int previous = -1;
    for (const std::uint8_t byte : in) {
        buffer = (buffer << 8) | byte;
        bufferBits += 8;
        while (bufferBits >= bits) {
            const int code = static_cast<int>((buffer >> (bufferBits - bits)) & ((1u << bits) - 1));
            bufferBits -= bits;
            if (code == 256) {
                reset();
                bits = 9;
                previous = -1;
                continue;
            }
            if (code == 257)
                return true;
            Bytes entry;
            if (code < static_cast<int>(table.size()) && !table[code].empty()) {
                entry = table[code];
            } else if (previous >= 0 && code == static_cast<int>(table.size())) {
                entry = table[previous];
                entry.push_back(table[previous][0]);
            } else {
                return false;
            }
            if (out.size() + entry.size() > kMaxStream)
                return false;
            out.insert(out.end(), entry.begin(), entry.end());
            if (previous >= 0 && table.size() < 4096) {
                Bytes added = table[previous];
                added.push_back(entry[0]);
                table.push_back(std::move(added));
            }
            previous = code;
            const auto size = static_cast<int>(table.size()) + earlyChange;
            if (size >= 2048)
                bits = 12;
            else if (size >= 1024)
                bits = 11;
            else if (size >= 512)
                bits = 10;
        }
    }
    return true;
}

// PNG predictors (10 and up), as xref and object streams use them.
bool unpredict(Bytes& data, const Object* parms)
{
    if (!parms || !parms->is(Object::Kind::Dictionary))
        return true;
    const Object* predictor = parms->get("Predictor");
    if (!predictor || predictor->number < 10)
        return true; // none, or TIFF's, which text streams do not use
    auto value = [&](std::string_view key, double fallback) {
        const Object* o = parms->get(key);
        return o && o->is(Object::Kind::Number) ? o->number : fallback;
    };
    const auto colors = static_cast<std::size_t>(std::clamp(value("Colors", 1), 1.0, 32.0));
    const auto bitsPer = static_cast<std::size_t>(std::clamp(value("BitsPerComponent", 8), 1.0, 16.0));
    const auto columns = static_cast<std::size_t>(std::clamp(value("Columns", 1), 1.0, 1e6));
    const std::size_t bpp = std::max<std::size_t>(1, colors * bitsPer / 8);
    const std::size_t rowSize = (colors * bitsPer * columns + 7) / 8;
    Bytes out;
    Bytes prior(rowSize, 0);
    for (std::size_t at = 0; at + 1 + rowSize <= data.size(); at += 1 + rowSize) {
        const std::uint8_t type = data[at];
        Bytes row(data.begin() + static_cast<std::ptrdiff_t>(at + 1),
                  data.begin() + static_cast<std::ptrdiff_t>(at + 1 + rowSize));
        for (std::size_t i = 0; i < rowSize; ++i) {
            const int left = i >= bpp ? row[i - bpp] : 0;
            const int up = prior[i];
            const int upLeft = i >= bpp ? prior[i - bpp] : 0;
            int add = 0;
            switch (type) {
            case 1:
                add = left;
                break;
            case 2:
                add = up;
                break;
            case 3:
                add = (left + up) / 2;
                break;
            case 4: {
                const int p = left + up - upLeft;
                const int pa = std::abs(p - left);
                const int pb = std::abs(p - up);
                const int pc = std::abs(p - upLeft);
                add = pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft;
                break;
            }
            default:
                break;
            }
            row[i] = static_cast<std::uint8_t>(row[i] + add);
        }
        out.insert(out.end(), row.begin(), row.end());
        prior = std::move(row);
    }
    data = std::move(out);
    return true;
}

// --- The document -------------------------------------------------------------------------------

struct Entry
{
    Object object;
    std::span<const std::uint8_t> stream; // raw stream bytes, if it is a stream
    bool hasStream = false;
};

class Document
{
public:
    explicit Document(std::span<const std::uint8_t> data) : m_data(data) { scan(); }

    bool encrypted() const { return m_encrypted; }

    const Object* resolve(const Object* o, int depth = 0) const
    {
        while (o && o->is(Object::Kind::Reference) && depth++ < 32) {
            const auto it = m_objects.find(o->objectNumber);
            o = it == m_objects.end() ? nullptr : &it->second.object;
        }
        return o;
    }

    const Object* get(const Object* dict, std::string_view key) const
    {
        dict = resolve(dict);
        return dict ? resolve(dict->get(key)) : nullptr;
    }

    // The entry an object refers to, for its stream.
    const Entry* entry(const Object* o) const
    {
        if (!o || !o->is(Object::Kind::Reference))
            return nullptr;
        const auto it = m_objects.find(o->objectNumber);
        return it == m_objects.end() ? nullptr : &it->second;
    }

    // A stream's bytes with its filters undone; nothing if a filter is not one read here.
    std::optional<Bytes> decode(const Entry& e) const
    {
        if (!e.hasStream)
            return std::nullopt;
        Bytes data(e.stream.begin(), e.stream.end());
        const Object* filter = get(&e.object, "Filter");
        const Object* parms = get(&e.object, "DecodeParms");
        std::vector<const Object*> filters;
        std::vector<const Object*> allParms;
        if (filter && filter->is(Object::Kind::Name)) {
            filters.push_back(filter);
            allParms.push_back(parms);
        } else if (filter && filter->is(Object::Kind::Array)) {
            for (std::size_t i = 0; i < filter->items.size(); ++i) {
                filters.push_back(resolve(&filter->items[i]));
                allParms.push_back(parms && parms->is(Object::Kind::Array) && i < parms->items.size()
                                       ? resolve(&parms->items[i])
                                       : nullptr);
            }
        }
        for (std::size_t i = 0; i < filters.size(); ++i) {
            const Object* f = filters[i];
            if (!f || !f->is(Object::Kind::Name))
                return std::nullopt;
            Bytes out;
            if (f->text == "FlateDecode" || f->text == "Fl") {
                // A broken stream keeps what came before the break; some lack the zlib header.
                if (!zlibInflate(data, kMaxStream, out) && out.empty())
                    inflate(data, kMaxStream, out);
                unpredict(out, allParms[i]);
            } else if (f->text == "LZWDecode" || f->text == "LZW") {
                const Object* early = allParms[i] ? allParms[i]->get("EarlyChange") : nullptr;
                lzw(data, early && early->number == 0 ? 0 : 1, out);
                unpredict(out, allParms[i]);
            } else if (f->text == "ASCIIHexDecode" || f->text == "AHx") {
                asciiHex(data, out);
            } else if (f->text == "ASCII85Decode" || f->text == "A85") {
                ascii85(data, out);
            } else {
                return std::nullopt; // images, or something not read here
            }
            data = std::move(out);
        }
        return data;
    }

    // The pages in order, each with the resources it inherits.
    struct Page
    {
        const Object* page = nullptr;
        const Object* resources = nullptr;
    };
    std::vector<Page> pages() const
    {
        std::vector<Page> result;
        std::set<const Object*> seen;
        if (const Object* root = rootPages())
            collectPages(root, nullptr, result, seen, 0);
        if (result.empty()) {
            // No page tree to be found: every page object, in the order of their numbers.
            std::map<int, const Object*> byNumber;
            for (const auto& [number, e] : m_objects) {
                if (const Object* type = e.object.get("Type"); type && type->isName("Page"))
                    byNumber.emplace(number, &e.object);
            }
            for (const auto& [number, page] : byNumber)
                result.push_back({page, get(page, "Resources")});
        }
        return result;
    }

private:
    const Object* rootPages() const
    {
        const Object* catalog = nullptr;
        if (m_root != 0) {
            const auto it = m_objects.find(m_root);
            if (it != m_objects.end())
                catalog = &it->second.object;
        }
        if (!catalog) {
            for (const auto& [number, e] : m_objects) {
                if (const Object* type = e.object.get("Type"); type && type->isName("Catalog"))
                    catalog = &e.object;
            }
        }
        return catalog ? get(catalog, "Pages") : nullptr;
    }

    void collectPages(const Object* node, const Object* resources, std::vector<Page>& out,
                      std::set<const Object*>& seen, int depth) const
    {
        node = resolve(node);
        if (!node || !node->is(Object::Kind::Dictionary) || depth > 64 || !seen.insert(node).second)
            return;
        if (const Object* own = get(node, "Resources"))
            resources = own;
        const Object* kids = get(node, "Kids");
        const Object* type = get(node, "Type");
        if (kids && kids->is(Object::Kind::Array) && !(type && type->isName("Page"))) {
            for (const Object& kid : kids->items)
                collectPages(&kid, resources, out, seen, depth + 1);
            return;
        }
        out.push_back({node, resources});
    }

    // Finds every "n g obj" in the file: later definitions replace earlier ones, as incremental
    // updates intend. Then the objects packed in object streams, where not defined directly.
    void scan()
    {
        const std::string_view text(reinterpret_cast<const char*>(m_data.data()), m_data.size());
        std::size_t at = 0;
        while ((at = text.find("obj", at)) != std::string_view::npos) {
            const std::size_t keywordEnd = at + 3;
            const std::size_t found = at;
            at = keywordEnd;
            if (keywordEnd < m_data.size() && !isWhite(m_data[keywordEnd]) && !isDelimiter(m_data[keywordEnd]))
                continue; // part of a longer word
            const auto number = objectNumberBefore(found);
            if (!number)
                continue;
            Lexer lexer(m_data, keywordEnd);
            Entry e;
            e.object = lexer.next();
            if (e.object.is(Object::Kind::End))
                continue;
            const std::size_t afterObject = lexer.pos();
            lexer.skipWhite();
            if (e.object.is(Object::Kind::Dictionary) && hasAt(text, lexer.pos(), "stream")) {
                std::size_t start = lexer.pos() + 6;
                if (start < m_data.size() && m_data[start] == '\r')
                    ++start;
                if (start < m_data.size() && m_data[start] == '\n')
                    ++start;
                std::size_t end = std::string_view::npos;
                if (const Object* length = e.object.get("Length"); length && length->is(Object::Kind::Number)) {
                    const auto declared = static_cast<std::size_t>(std::max(0.0, length->number));
                    if (start + declared <= m_data.size()) {
                        Lexer check(m_data, start + declared);
                        check.skipWhite();
                        if (hasAt(text, check.pos(), "endstream"))
                            end = start + declared;
                    }
                }
                if (end == std::string_view::npos) {
                    end = text.find("endstream", start);
                    if (end == std::string_view::npos)
                        end = m_data.size();
                    // The end of line before "endstream" is not part of the data.
                    if (end > start && m_data[end - 1] == '\n')
                        --end;
                    if (end > start && m_data[end - 1] == '\r')
                        --end;
                }
                e.stream = m_data.subspan(start, end - start);
                e.hasStream = true;
                at = end;
            } else {
                at = std::max(at, afterObject);
            }
            if (const Object* type = e.object.get("Type")) {
                if (type->isName("XRef"))
                    noteTrailer(e.object);
                else if (type->isName("ObjStm"))
                    m_objectStreams.push_back(*number);
            }
            m_objects[*number] = std::move(e);
        }

        // Classic trailers.
        at = 0;
        while ((at = text.find("trailer", at)) != std::string_view::npos) {
            Lexer lexer(m_data, at + 7);
            const Object trailer = lexer.next();
            noteTrailer(trailer);
            at += 7;
        }

        for (const int number : m_objectStreams) {
            const auto it = m_objects.find(number);
            if (it == m_objects.end())
                continue;
            const auto decoded = decode(it->second);
            if (!decoded)
                continue;
            const Object* count = it->second.object.get("N");
            const Object* first = it->second.object.get("First");
            if (!count || !first)
                continue;
            m_packed.push_back(std::make_unique<Bytes>(*decoded));
            const Bytes& bytes = *m_packed.back();
            Lexer header(bytes);
            header.setAllowReferences(false);
            const auto n = static_cast<int>(std::clamp(count->number, 0.0, 1e6));
            const auto base = static_cast<std::size_t>(std::max(0.0, first->number));
            for (int i = 0; i < n; ++i) {
                const Object objectNumber = header.next();
                const Object offset = header.next();
                if (!objectNumber.is(Object::Kind::Number) || !offset.is(Object::Kind::Number))
                    break;
                const auto key = static_cast<int>(objectNumber.number);
                if (m_objects.contains(key))
                    continue;
                const std::size_t pos = base + static_cast<std::size_t>(std::max(0.0, offset.number));
                if (pos >= bytes.size())
                    continue;
                Lexer lexer(bytes, pos);
                Entry e;
                e.object = lexer.next();
                m_objects.emplace(key, std::move(e));
            }
        }
    }

    // The object number in "12 0 obj", read back from the keyword at `at`.
    std::optional<int> objectNumberBefore(std::size_t at) const
    {
        std::size_t i = at;
        auto skipBack = [&] {
            while (i > 0 && isWhite(m_data[i - 1]))
                --i;
        };
        auto digitsBack = [&] {
            const std::size_t end = i;
            while (i > 0 && m_data[i - 1] >= '0' && m_data[i - 1] <= '9')
                --i;
            return end - i;
        };
        if (i == 0 || !isWhite(m_data[i - 1]))
            return std::nullopt;
        skipBack();
        if (digitsBack() == 0 || i == 0 || !isWhite(m_data[i - 1]))
            return std::nullopt;
        skipBack();
        const std::size_t end = i;
        const std::size_t length = digitsBack();
        if (length == 0 || length > 9 || (i > 0 && !isWhite(m_data[i - 1]) && !isDelimiter(m_data[i - 1])))
            return std::nullopt;
        int value = 0;
        for (std::size_t k = i; k < end; ++k)
            value = value * 10 + (m_data[k] - '0');
        return value;
    }

    void noteTrailer(const Object& trailer)
    {
        if (!trailer.is(Object::Kind::Dictionary))
            return;
        if (trailer.get("Encrypt"))
            m_encrypted = true;
        if (const Object* root = trailer.get("Root"); root && root->is(Object::Kind::Reference))
            m_root = root->objectNumber;
    }

    std::span<const std::uint8_t> m_data;
    std::unordered_map<int, Entry> m_objects;
    std::vector<int> m_objectStreams;
    std::vector<std::unique_ptr<Bytes>> m_packed; // the decoded object streams, kept alive
    int m_root = 0;
    bool m_encrypted = false;
};

// --- Fonts --------------------------------------------------------------------------------------

std::u32string utf16ToCodePoints(std::string_view bytes)
{
    std::u32string out;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        char32_t unit = (static_cast<std::uint8_t>(bytes[i]) << 8) | static_cast<std::uint8_t>(bytes[i + 1]);
        if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < bytes.size()) {
            const char32_t low = (static_cast<std::uint8_t>(bytes[i + 2]) << 8) | static_cast<std::uint8_t>(bytes[i + 3]);
            if (low >= 0xDC00 && low < 0xE000) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        out += unit;
    }
    if (bytes.size() == 1)
        out += static_cast<std::uint8_t>(bytes[0]);
    return out;
}

std::uint32_t codeOf(std::string_view bytes)
{
    std::uint32_t code = 0;
    for (const char c : bytes.substr(0, 4))
        code = (code << 8) | static_cast<std::uint8_t>(c);
    return code;
}

// Characters for the glyph names fonts use in their /Differences, where they are not "uniXXXX".
char32_t glyphNameToCodePoint(std::string_view name)
{
    static const std::unordered_map<std::string_view, char32_t> names = {
        {"space", U' '},         {"exclam", U'!'},        {"quotedbl", U'"'},       {"numbersign", U'#'},
        {"dollar", U'$'},        {"percent", U'%'},       {"ampersand", U'&'},      {"quotesingle", U'\''},
        {"parenleft", U'('},     {"parenright", U')'},    {"asterisk", U'*'},       {"plus", U'+'},
        {"comma", U','},         {"hyphen", U'-'},        {"period", U'.'},         {"slash", U'/'},
        {"zero", U'0'},          {"one", U'1'},           {"two", U'2'},            {"three", U'3'},
        {"four", U'4'},          {"five", U'5'},          {"six", U'6'},            {"seven", U'7'},
        {"eight", U'8'},         {"nine", U'9'},          {"colon", U':'},          {"semicolon", U';'},
        {"less", U'<'},          {"equal", U'='},         {"greater", U'>'},        {"question", U'?'},
        {"at", U'@'},            {"bracketleft", U'['},   {"backslash", U'\\'},     {"bracketright", U']'},
        {"asciicircum", U'^'},   {"underscore", U'_'},    {"grave", U'`'},          {"braceleft", U'{'},
        {"bar", U'|'},           {"braceright", U'}'},    {"asciitilde", U'~'},     {"quoteleft", U'‘'},
        {"quoteright", U'’'}, {"quotedblleft", U'“'}, {"quotedblright", U'”'},
        {"quotesinglbase", U'‚'}, {"quotedblbase", U'„'}, {"endash", U'–'}, {"emdash", U'—'},
        {"bullet", U'•'},   {"ellipsis", U'…'}, {"dagger", U'†'},    {"daggerdbl", U'‡'},
        {"fi", U'ﬁ'},       {"fl", U'ﬂ'},       {"ff", U'ﬀ'},        {"ffi", U'ﬃ'},
        {"ffl", U'ﬄ'},      {"trademark", U'™'}, {"copyright", U'©'}, {"registered", U'®'},
        {"degree", U'°'},   {"section", U'§'},  {"paragraph", U'¶'}, {"minus", U'−'},
        {"multiply", U'×'}, {"divide", U'÷'},   {"Euro", U'€'},      {"sterling", U'£'},
        {"yen", U'¥'},      {"cent", U'¢'},     {"periodcentered", U'·'}, {"nbspace", U' '},
        {"guillemotleft", U'«'}, {"guillemotright", U'»'}, {"guilsinglleft", U'‹'},
        {"guilsinglright", U'›'}, {"perthousand", U'‰'}, {"florin", U'ƒ'}, {"dotlessi", U'ı'},
        {"exclamdown", U'¡'}, {"questiondown", U'¿'}, {"plusminus", U'±'}, {"mu", U'µ'},
    };
    if (name.size() == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z')))
        return static_cast<char32_t>(name[0]);
    const std::string_view base = name.substr(0, name.find('.')); // "a.sc" is still an a
    if (const auto it = names.find(base); it != names.end())
        return it->second;
    auto hex = [](std::string_view digits) -> char32_t {
        char32_t value = 0;
        for (const char c : digits) {
            const int v = hexValue(static_cast<std::uint8_t>(c));
            if (v < 0)
                return 0;
            value = value * 16 + static_cast<char32_t>(v);
        }
        return value;
    };
    if (base.starts_with("uni") && base.size() == 7)
        return hex(base.substr(3));
    if (base.starts_with("u") && base.size() >= 5 && base.size() <= 7)
        return hex(base.substr(1));
    if (base.size() == 1 && static_cast<std::uint8_t>(base[0]) < 0x80)
        return static_cast<char32_t>(base[0]);
    return 0;
}

// WinAnsiEncoding: Latin-1, but for 0x80–0x9F.
char32_t winAnsi(std::uint8_t code)
{
    static constexpr std::array<char32_t, 32> high = {
        0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
    if (code >= 0x80 && code < 0xA0)
        return high[code - 0x80];
    return code;
}

struct Font
{
    bool composite = false;             // Type0: codes of one or more bytes, glyphs by CID
    bool unicodeCodes = false;          // Type0 with a Uni…-UCS2 or -UTF16 CMap: codes are UTF-16
    std::vector<int> codeLengths;       // byte lengths codes may have, from the ToUnicode CMap
    std::unordered_map<std::uint32_t, std::u32string> toUnicode;
    std::array<char32_t, 256> encoding{}; // simple fonts
    bool hasEncoding = false;
    // Widths, in thousandths of the size.
    int firstChar = 0;
    std::vector<double> widths;
    std::unordered_map<std::uint32_t, double> cidWidths;
    double defaultWidth = 1000;
    double missingWidth = 0;

    double width(std::uint32_t code) const
    {
        if (composite) {
            const auto it = cidWidths.find(code);
            return it == cidWidths.end() ? defaultWidth : it->second;
        }
        const auto index = static_cast<std::int64_t>(code) - firstChar;
        if (index >= 0 && index < static_cast<std::int64_t>(widths.size()))
            return widths[static_cast<std::size_t>(index)];
        return missingWidth > 0 ? missingWidth : 500;
    }
};

void parseCMap(std::span<const std::uint8_t> data, Font& font)
{
    Lexer lexer(data);
    lexer.setAllowReferences(false);
    std::vector<Object> operands;
    auto addRange = [&](const std::string& low, const std::string& high, const Object& destination) {
        const std::uint32_t from = codeOf(low);
        const std::uint32_t to = codeOf(high);
        if (to < from || to - from > 0xFFFF)
            return;
        if (destination.is(Object::Kind::Array)) {
            for (std::uint32_t code = from; code <= to && code - from < destination.items.size(); ++code)
                font.toUnicode[code] = utf16ToCodePoints(destination.items[code - from].text);
            return;
        }
        std::u32string base = utf16ToCodePoints(destination.text);
        if (base.empty())
            return;
        for (std::uint32_t code = from; code <= to; ++code) {
            font.toUnicode[code] = base;
            ++base.back();
        }
    };
    for (Object o = lexer.next(); !o.is(Object::Kind::End); o = lexer.next()) {
        if (!o.is(Object::Kind::Keyword)) {
            operands.push_back(std::move(o));
            continue;
        }
        if (o.text == "endcodespacerange") {
            for (std::size_t i = 0; i + 1 < operands.size(); i += 2) {
                const auto length = static_cast<int>(operands[i].text.size());
                if (length >= 1 && length <= 4 &&
                    std::find(font.codeLengths.begin(), font.codeLengths.end(), length) == font.codeLengths.end())
                    font.codeLengths.push_back(length);
            }
        } else if (o.text == "endbfchar") {
            for (std::size_t i = 0; i + 1 < operands.size(); i += 2)
                font.toUnicode[codeOf(operands[i].text)] = utf16ToCodePoints(operands[i + 1].text);
        } else if (o.text == "endbfrange") {
            for (std::size_t i = 0; i + 2 < operands.size(); i += 3)
                addRange(operands[i].text, operands[i + 1].text, operands[i + 2]);
        }
        operands.clear();
    }
    std::sort(font.codeLengths.begin(), font.codeLengths.end());
}

Font loadFont(const Document& doc, const Object* fontRef)
{
    Font font;
    const Object* dict = doc.resolve(fontRef);
    if (!dict)
        return font;
    const Object* subtype = doc.get(dict, "Subtype");
    font.composite = subtype && subtype->isName("Type0");

    if (const Entry* cmap = doc.entry(dict->get("ToUnicode"))) {
        if (const auto bytes = doc.decode(*cmap))
            parseCMap(*bytes, font);
    }

    if (font.composite) {
        if (const Object* encoding = doc.get(dict, "Encoding"); encoding && encoding->is(Object::Kind::Name))
            font.unicodeCodes = encoding->text.find("UCS2") != std::string::npos ||
                                encoding->text.find("UTF16") != std::string::npos;
        const Object* descendants = doc.get(dict, "DescendantFonts");
        const Object* cidFont =
            descendants && descendants->is(Object::Kind::Array) && !descendants->items.empty()
                ? doc.resolve(&descendants->items[0])
                : nullptr;
        if (const Object* dw = doc.get(cidFont, "DW"); dw && dw->is(Object::Kind::Number))
            font.defaultWidth = dw->number;
        // /W [c [w1 w2 ...] cFirst cLast w ...]
        if (const Object* w = doc.get(cidFont, "W"); w && w->is(Object::Kind::Array)) {
            const auto& items = w->items;
            for (std::size_t i = 0; i + 1 < items.size();) {
                const Object* first = doc.resolve(&items[i]);
                const Object* second = doc.resolve(&items[i + 1]);
                if (!first || !second || !first->is(Object::Kind::Number))
                    break;
                const auto start = static_cast<std::uint32_t>(std::max(0.0, first->number));
                if (second->is(Object::Kind::Array)) {
                    for (std::size_t k = 0; k < second->items.size() && k < 65536; ++k) {
                        if (const Object* v = doc.resolve(&second->items[k]); v && v->is(Object::Kind::Number))
                            font.cidWidths[start + static_cast<std::uint32_t>(k)] = v->number;
                    }
                    i += 2;
                } else if (i + 2 < items.size()) {
                    const Object* width = doc.resolve(&items[i + 2]);
                    const auto end = static_cast<std::uint32_t>(std::max(0.0, second->number));
                    for (std::uint32_t c = start; c <= end && c - start < 65536; ++c)
                        font.cidWidths[c] = width && width->is(Object::Kind::Number) ? width->number : 1000;
                    i += 3;
                } else {
                    break;
                }
            }
        }
        if (font.codeLengths.empty())
            font.codeLengths = {2};
        return font;
    }

    // A simple font: one byte a code, its encoding a base and differences from it.
    for (int c = 0; c < 256; ++c)
        font.encoding[c] = winAnsi(static_cast<std::uint8_t>(c));
    const Object* encoding = doc.get(dict, "Encoding");
    if (encoding && encoding->is(Object::Kind::Dictionary)) {
        if (const Object* differences = doc.get(encoding, "Differences");
            differences && differences->is(Object::Kind::Array)) {
            int code = 0;
            for (const Object& item : differences->items) {
                if (item.is(Object::Kind::Number)) {
                    code = static_cast<int>(item.number);
                } else if (item.is(Object::Kind::Name) && code >= 0 && code < 256) {
                    if (const char32_t c = glyphNameToCodePoint(item.text))
                        font.encoding[code] = c;
                    ++code;
                }
            }
        }
    }
    font.hasEncoding = true;
    if (const Object* first = doc.get(dict, "FirstChar"); first && first->is(Object::Kind::Number))
        font.firstChar = static_cast<int>(first->number);
    if (const Object* widths = doc.get(dict, "Widths"); widths && widths->is(Object::Kind::Array)) {
        for (const Object& w : widths->items) {
            const Object* v = doc.resolve(&w);
            font.widths.push_back(v && v->is(Object::Kind::Number) ? v->number : 0);
        }
    }
    if (const Object* descriptor = doc.get(dict, "FontDescriptor")) {
        if (const Object* missing = doc.get(descriptor, "MissingWidth"); missing && missing->is(Object::Kind::Number))
            font.missingWidth = missing->number;
    }
    font.codeLengths = {1};
    return font;
}

// --- Content ------------------------------------------------------------------------------------

bool isCjk(char32_t c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF);
}

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x110000) {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

struct Matrix
{
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    // this × m: this applied first.
    Matrix times(const Matrix& m) const
    {
        return {a * m.a + b * m.c,       a * m.b + b * m.d,       c * m.a + d * m.c,
                c * m.b + d * m.d,       e * m.a + f * m.c + m.e, e * m.b + f * m.d + m.f};
    }
};

class PageReader
{
public:
    PageReader(const Document& doc, std::string& out, std::size_t maxText, std::size_t& unmapped)
        : m_doc(doc), m_out(out), m_maxText(maxText), m_unmapped(unmapped)
    {
    }

    void read(std::span<const std::uint8_t> content, const Object* resources, const Matrix& ctm, int depth)
    {
        Lexer lexer(content);
        lexer.setAllowReferences(false);
        std::vector<Object> operands;
        Matrix graphics = ctm;
        std::vector<Matrix> stack;
        for (Object o = lexer.next(); !o.is(Object::Kind::End); o = lexer.next()) {
            if (m_out.size() > m_maxText)
                return;
            if (!o.is(Object::Kind::Keyword)) {
                if (operands.size() < 64)
                    operands.push_back(std::move(o));
                continue;
            }
            const std::string& op = o.text;
            auto num = [&](std::size_t i) {
                return i < operands.size() && operands[i].is(Object::Kind::Number) ? operands[i].number : 0.0;
            };
            if (op == "BT") {
                m_text = m_line = Matrix{};
            } else if (op == "q") {
                if (stack.size() < 64)
                    stack.push_back(graphics);
            } else if (op == "Q") {
                if (!stack.empty()) {
                    graphics = stack.back();
                    stack.pop_back();
                }
            } else if (op == "cm" && operands.size() >= 6) {
                graphics = Matrix{num(0), num(1), num(2), num(3), num(4), num(5)}.times(graphics);
            } else if (op == "Tf" && operands.size() >= 2) {
                m_fontSize = num(1);
                m_font = font(resources, operands[0]);
            } else if (op == "Tc") {
                m_charSpacing = num(0);
            } else if (op == "Tw") {
                m_wordSpacing = num(0);
            } else if (op == "Tz") {
                m_scale = num(0) / 100;
            } else if (op == "TL") {
                m_leading = num(0);
            } else if (op == "Td" || op == "TD") {
                if (op == "TD")
                    m_leading = -num(1);
                m_line = Matrix{1, 0, 0, 1, num(0), num(1)}.times(m_line);
                m_text = m_line;
            } else if (op == "Tm" && operands.size() >= 6) {
                m_line = m_text = Matrix{num(0), num(1), num(2), num(3), num(4), num(5)};
            } else if (op == "T*") {
                nextLine();
            } else if (op == "Tj" && !operands.empty()) {
                show(operands[0].text, graphics);
            } else if (op == "'" && !operands.empty()) {
                nextLine();
                show(operands[0].text, graphics);
            } else if (op == "\"" && operands.size() >= 3) {
                m_wordSpacing = num(0);
                m_charSpacing = num(1);
                nextLine();
                show(operands[2].text, graphics);
            } else if (op == "TJ" && !operands.empty() && operands[0].is(Object::Kind::Array)) {
                for (const Object& item : operands[0].items) {
                    if (item.is(Object::Kind::String))
                        show(item.text, graphics);
                    else if (item.is(Object::Kind::Number))
                        advance(-item.number / 1000 * m_fontSize * m_scale);
                }
            } else if (op == "Do" && !operands.empty() && depth < kMaxFormDepth) {
                form(resources, operands[0], graphics, depth);
            } else if (op == "BI") {
                skipInlineImage(lexer, content);
            }
            operands.clear();
        }
    }

    // Between pages: what came before ends its line.
    void endPage()
    {
        while (!m_out.empty() && (m_out.back() == ' ' || m_out.back() == '\n'))
            m_out.pop_back();
        if (!m_out.empty())
            m_out += "\n\n";
        m_hasPosition = false;
    }

private:
    const Font* font(const Object* resources, const Object& name)
    {
        if (!name.is(Object::Kind::Name))
            return nullptr;
        const Object* fonts = m_doc.get(resources, "Font");
        const Object* ref = fonts ? fonts->get(name.text) : nullptr;
        if (!ref)
            return nullptr;
        const Object* key = m_doc.resolve(ref);
        if (!key)
            return nullptr;
        auto it = m_fonts.find(key);
        if (it == m_fonts.end())
            it = m_fonts.emplace(key, loadFont(m_doc, ref)).first;
        return &it->second;
    }

    void form(const Object* resources, const Object& name, const Matrix& graphics, int depth)
    {
        if (!name.is(Object::Kind::Name))
            return;
        const Object* xobjects = m_doc.get(resources, "XObject");
        const Object* ref = xobjects ? xobjects->get(name.text) : nullptr;
        const Entry* entry = m_doc.entry(ref);
        if (!entry)
            return;
        const Object* subtype = m_doc.get(&entry->object, "Subtype");
        if (!subtype || !subtype->isName("Form"))
            return;
        const auto content = m_doc.decode(*entry);
        if (!content)
            return;
        Matrix matrix = graphics;
        if (const Object* m = m_doc.get(&entry->object, "Matrix"); m && m->is(Object::Kind::Array) && m->items.size() == 6) {
            auto v = [&](std::size_t i) { return m->items[i].is(Object::Kind::Number) ? m->items[i].number : 0.0; };
            matrix = Matrix{v(0), v(1), v(2), v(3), v(4), v(5)}.times(graphics);
        }
        const Object* own = m_doc.get(&entry->object, "Resources");
        // The form has its own text state; the page's resumes after it.
        const Matrix text = m_text;
        const Matrix line = m_line;
        read(*content, own ? own : resources, matrix, depth + 1);
        m_text = text;
        m_line = line;
    }

    static void skipInlineImage(Lexer& lexer, std::span<const std::uint8_t> content)
    {
        // The dictionary, up to ID; then binary data up to EI between white space.
        for (Object o = lexer.next(); !o.is(Object::Kind::End); o = lexer.next()) {
            if (o.is(Object::Kind::Keyword) && o.text == "ID")
                break;
        }
        std::size_t at = lexer.pos() + 1;
        while (at + 2 <= content.size()) {
            if (content[at] == 'E' && content[at + 1] == 'I' && isWhite(content[at - 1]) &&
                (at + 2 == content.size() || isWhite(content[at + 2]))) {
                lexer.setPos(at + 2);
                return;
            }
            ++at;
        }
        lexer.setPos(content.size());
    }

    void nextLine()
    {
        m_line = Matrix{1, 0, 0, 1, 0, -m_leading}.times(m_line);
        m_text = m_line;
    }

    void advance(double tx) { m_text = Matrix{1, 0, 0, 1, tx, 0}.times(m_text); }

    // Where the text space's origin is now, on the page.
    std::pair<double, double> position(const Matrix& graphics) const
    {
        const Matrix m = m_text.times(graphics);
        return {m.e, m.f};
    }

    double effectiveSize(const Matrix& graphics) const
    {
        const Matrix m = m_text.times(graphics);
        const double scale = std::sqrt(m.c * m.c + m.d * m.d);
        return std::max(1.0, std::abs(m_fontSize) * (scale > 0 ? scale : 1));
    }

    void show(const std::string& bytes, const Matrix& graphics)
    {
        if (!m_font)
            return;
        const double size = effectiveSize(graphics);
        std::size_t i = 0;
        while (i < bytes.size()) {
            // The code: the shortest length the font's code space has that is defined.
            int length = m_font->codeLengths.empty() ? 1 : m_font->codeLengths.front();
            if (m_font->codeLengths.size() > 1) {
                for (const int candidate : m_font->codeLengths) {
                    if (i + static_cast<std::size_t>(candidate) <= bytes.size() &&
                        m_font->toUnicode.contains(codeOf(std::string_view(bytes).substr(i, candidate)))) {
                        length = candidate;
                        break;
                    }
                }
            }
            length = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(length), bytes.size() - i));
            const std::string_view codeBytes = std::string_view(bytes).substr(i, static_cast<std::size_t>(length));
            i += static_cast<std::size_t>(length);
            const std::uint32_t code = codeOf(codeBytes);

            std::u32string chars;
            if (const auto it = m_font->toUnicode.find(code); it != m_font->toUnicode.end())
                chars = it->second;
            else if (m_font->composite && m_font->unicodeCodes)
                chars = utf16ToCodePoints(codeBytes);
            else if (!m_font->composite && m_font->hasEncoding)
                chars = std::u32string(1, m_font->encoding[code & 0xFF]);

            const auto [x, y] = position(graphics);
            if (!chars.empty() && chars.front() != 0) {
                place(x, y, size, chars.front());
                for (const char32_t c : chars) {
                    if (c >= 0x20 || c == '\t')
                        appendUtf8(m_out, c == '\t' ? U' ' : c);
                }
                m_lastChar = chars.back();
            } else {
                ++m_unmapped;
            }

            // Where the glyph ends, before the spacing after it: writers such as groff put the
            // spaces between words there.
            advance(m_font->width(code) / 1000 * m_fontSize * m_scale);
            const auto [endX, endY] = position(graphics);
            m_lastX = endX;
            m_lastY = endY;
            m_lastSize = size;
            m_hasPosition = true;
            double spacing = m_charSpacing;
            if (length == 1 && code == 32)
                spacing += m_wordSpacing;
            advance(spacing * m_scale);
        }
    }

    // Starts a line, or puts a space, if the glyph at (x, y) is not where the text left off.
    void place(double x, double y, double size, char32_t next)
    {
        if (!m_hasPosition || m_out.empty())
            return;
        const double lineSize = std::max(size, m_lastSize);
        const double dy = y - m_lastY;
        const double dx = x - m_lastX;
        if (std::abs(dy) > lineSize * 0.5 || dx < -lineSize * 4) {
            while (!m_out.empty() && m_out.back() == ' ')
                m_out.pop_back();
            if (!m_out.empty() && m_out.back() != '\n')
                m_out += '\n';
            return;
        }
        // Chinese and Japanese text has no spaces; justified lines leave gaps between glyphs.
        const bool cjk = isCjk(next) || isCjk(m_lastChar);
        if (dx > lineSize * (cjk ? 0.6 : 0.18) && m_out.back() != ' ' && m_out.back() != '\n' && next != U' ')
            m_out += ' ';
    }

    const Document& m_doc;
    std::string& m_out;
    std::size_t m_maxText;
    std::size_t& m_unmapped;
    std::map<const Object*, Font> m_fonts;
    const Font* m_font = nullptr;
    double m_fontSize = 0;
    double m_charSpacing = 0;
    double m_wordSpacing = 0;
    double m_scale = 1;
    double m_leading = 0;
    Matrix m_text;
    Matrix m_line;
    bool m_hasPosition = false;
    double m_lastX = 0;
    double m_lastY = 0;
    double m_lastSize = 0;
    char32_t m_lastChar = 0;
};

} // namespace

PdfText extractPdfText(std::span<const std::uint8_t> data, std::size_t maxText)
{
    PdfText result;
    const Document doc(data);
    if (doc.encrypted()) {
        result.status = PdfText::Status::Encrypted;
        return result;
    }
    const auto pages = doc.pages();
    result.pages = static_cast<int>(pages.size());
    PageReader reader(doc, result.utf8, maxText, result.unmappedGlyphs);
    for (const auto& page : pages) {
        if (result.utf8.size() > maxText)
            break;
        const Object* contents = page.page->get("Contents");
        std::vector<const Object*> parts;
        if (const Object* resolved = doc.resolve(contents); resolved && resolved->is(Object::Kind::Array)) {
            for (const Object& part : resolved->items)
                parts.push_back(&part);
        } else if (contents) {
            parts.push_back(contents);
        }
        // The parts of a page's content are one stream cut in pieces, maybe mid-operator.
        Bytes content;
        for (const Object* part : parts) {
            if (const Entry* entry = doc.entry(part)) {
                if (const auto bytes = doc.decode(*entry)) {
                    content.insert(content.end(), bytes->begin(), bytes->end());
                    content.push_back('\n');
                }
            }
        }
        reader.read(content, page.resources, Matrix{}, 0);
        reader.endPage();
    }
    while (!result.utf8.empty() && (result.utf8.back() == '\n' || result.utf8.back() == ' '))
        result.utf8.pop_back();
    if (!result.utf8.empty())
        result.utf8 += '\n';
    if (result.utf8.size() > maxText) {
        // Cut at the last whole character before the limit.
        std::size_t cut = maxText;
        while (cut > 0 && (static_cast<std::uint8_t>(result.utf8[cut]) & 0xC0) == 0x80)
            --cut;
        result.utf8.resize(cut);
        result.status = PdfText::Status::Truncated;
    } else if (result.utf8.empty()) {
        result.status = PdfText::Status::NoText;
    }
    return result;
}

} // namespace aiedit::core
