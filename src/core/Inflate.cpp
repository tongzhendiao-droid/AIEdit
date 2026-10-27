#include "core/Inflate.h"

#include <array>

namespace aiedit::core {

namespace {

constexpr int kMaxBits = 15;

// A canonical Huffman code: how many codes each length has, and the symbols in code order.
struct Huffman
{
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::array<std::uint16_t, 288> symbol{};
};

// Builds the code from the length of each symbol's code. False if it is over-subscribed (an
// incomplete code is allowed, as DEFLATE allows one for a single distance code).
bool buildHuffman(Huffman& h, const std::uint8_t* lengths, int n)
{
    h.count.fill(0);
    for (int i = 0; i < n; ++i)
        ++h.count[lengths[i]];
    if (h.count[0] == n)
        return true; // no codes: fine until one is decoded
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0)
            return false;
    }
    std::array<std::uint16_t, kMaxBits + 1> offsets{};
    for (int len = 1; len < kMaxBits; ++len)
        offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + h.count[len]);
    for (int i = 0; i < n; ++i) {
        if (lengths[i] != 0)
            h.symbol[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }
    return true;
}

constexpr std::array<std::uint16_t, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                         33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                         1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class Inflater
{
public:
    Inflater(std::span<const std::uint8_t> in, std::size_t maxSize, std::vector<std::uint8_t>& out)
        : m_in(in), m_maxSize(maxSize), m_out(out), m_start(out.size())
    {
    }

    bool run()
    {
        bool last = false;
        while (!last) {
            last = bits(1) != 0;
            const unsigned type = bits(2);
            bool ok = false;
            if (type == 0)
                ok = stored();
            else if (type == 1)
                ok = fixed();
            else if (type == 2)
                ok = dynamic();
            if (!ok || m_error)
                return false;
        }
        return true;
    }

    std::size_t consumed() const { return m_pos; }

private:
    unsigned bits(int need)
    {
        std::uint32_t value = m_bitBuffer;
        while (m_bitCount < need) {
            if (m_pos >= m_in.size()) {
                m_error = true;
                return 0;
            }
            value |= static_cast<std::uint32_t>(m_in[m_pos++]) << m_bitCount;
            m_bitCount += 8;
        }
        m_bitBuffer = value >> need;
        m_bitCount -= need;
        return value & ((1u << need) - 1);
    }

    bool put(std::uint8_t byte)
    {
        if (m_out.size() - m_start >= m_maxSize)
            return false;
        m_out.push_back(byte);
        return true;
    }

    bool stored()
    {
        m_bitBuffer = 0; // to the byte boundary
        m_bitCount = 0;
        if (m_pos + 4 > m_in.size())
            return false;
        const unsigned length = m_in[m_pos] | (m_in[m_pos + 1] << 8);
        const unsigned check = m_in[m_pos + 2] | (m_in[m_pos + 3] << 8);
        m_pos += 4;
        if (length != (~check & 0xFFFF) || m_pos + length > m_in.size())
            return false;
        if (m_out.size() - m_start + length > m_maxSize)
            return false;
        m_out.insert(m_out.end(), m_in.begin() + static_cast<std::ptrdiff_t>(m_pos),
                     m_in.begin() + static_cast<std::ptrdiff_t>(m_pos + length));
        m_pos += length;
        return true;
    }

    int decode(const Huffman& h)
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= static_cast<int>(bits(1));
            if (m_error)
                return -1;
            const int count = h.count[len];
            if (code - count < first)
                return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool codes(const Huffman& lengthCode, const Huffman& distanceCode)
    {
        for (;;) {
            const int symbol = decode(lengthCode);
            if (symbol < 0)
                return false;
            if (symbol < 256) {
                if (!put(static_cast<std::uint8_t>(symbol)))
                    return false;
                continue;
            }
            if (symbol == 256)
                return true;
            const int lengthIndex = symbol - 257;
            if (lengthIndex >= 29)
                return false;
            const std::size_t length = kLengthBase[lengthIndex] + bits(kLengthExtra[lengthIndex]);
            const int distanceSymbol = decode(distanceCode);
            if (distanceSymbol < 0 || distanceSymbol >= 30)
                return false;
            const std::size_t distance = kDistanceBase[distanceSymbol] + bits(kDistanceExtra[distanceSymbol]);
            if (m_error || distance > m_out.size() - m_start)
                return false;
            if (m_out.size() - m_start + length > m_maxSize)
                return false;
            const std::size_t from = m_out.size() - distance;
            for (std::size_t i = 0; i < length; ++i)
                m_out.push_back(m_out[from + i]); // may overlap what it writes: byte by byte
        }
    }

    bool fixed()
    {
        static const auto tables = [] {
            std::array<std::uint8_t, 288 + 30> lengths{};
            for (int i = 0; i < 144; ++i)
                lengths[i] = 8;
            for (int i = 144; i < 256; ++i)
                lengths[i] = 9;
            for (int i = 256; i < 280; ++i)
                lengths[i] = 7;
            for (int i = 280; i < 288; ++i)
                lengths[i] = 8;
            for (int i = 288; i < 288 + 30; ++i)
                lengths[i] = 5;
            std::pair<Huffman, Huffman> codes;
            buildHuffman(codes.first, lengths.data(), 288);
            buildHuffman(codes.second, lengths.data() + 288, 30);
            return codes;
        }();
        return codes(tables.first, tables.second);
    }

    bool dynamic()
    {
        static constexpr std::array<std::uint8_t, 19> kOrder = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                                11, 4,  12, 3, 13, 2, 14, 1, 15};
        const int lengthCount = static_cast<int>(bits(5)) + 257;
        const int distanceCount = static_cast<int>(bits(5)) + 1;
        const int codeCount = static_cast<int>(bits(4)) + 4;
        if (m_error || lengthCount > 286 || distanceCount > 30)
            return false;

        std::array<std::uint8_t, 320> lengths{};
        for (int i = 0; i < codeCount; ++i)
            lengths[kOrder[i]] = static_cast<std::uint8_t>(bits(3));
        Huffman codeLengths;
        if (m_error || !buildHuffman(codeLengths, lengths.data(), 19))
            return false;

        lengths.fill(0);
        int index = 0;
        while (index < lengthCount + distanceCount) {
            const int symbol = decode(codeLengths);
            if (symbol < 0)
                return false;
            if (symbol < 16) {
                lengths[index++] = static_cast<std::uint8_t>(symbol);
                continue;
            }
            std::uint8_t value = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0)
                    return false;
                value = lengths[index - 1];
                repeat = 3 + static_cast<int>(bits(2));
            } else if (symbol == 17) {
                repeat = 3 + static_cast<int>(bits(3));
            } else {
                repeat = 11 + static_cast<int>(bits(7));
            }
            if (m_error || index + repeat > lengthCount + distanceCount)
                return false;
            while (repeat-- > 0)
                lengths[index++] = value;
        }
        if (lengths[256] == 0)
            return false; // no end-of-block code

        Huffman lengthCode;
        Huffman distanceCode;
        if (!buildHuffman(lengthCode, lengths.data(), lengthCount) ||
            !buildHuffman(distanceCode, lengths.data() + lengthCount, distanceCount))
            return false;
        return codes(lengthCode, distanceCode);
    }

    std::span<const std::uint8_t> m_in;
    std::size_t m_pos = 0;
    std::uint32_t m_bitBuffer = 0;
    int m_bitCount = 0;
    bool m_error = false;
    std::size_t m_maxSize;
    std::vector<std::uint8_t>& m_out;
    std::size_t m_start;
};

} // namespace

bool inflate(std::span<const std::uint8_t> data, std::size_t maxSize, std::vector<std::uint8_t>& out)
{
    return Inflater(data, maxSize, out).run();
}

bool zlibInflate(std::span<const std::uint8_t> data, std::size_t maxSize, std::vector<std::uint8_t>& out)
{
    if (data.size() < 2)
        return false;
    const unsigned cmf = data[0];
    const unsigned flags = data[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flags) % 31 != 0 || (flags & 0x20) != 0)
        return false; // not DEFLATE, a bad header, or a preset dictionary
    return inflate(data.subspan(2), maxSize, out);
}

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const std::uint8_t byte : data)
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

} // namespace aiedit::core
