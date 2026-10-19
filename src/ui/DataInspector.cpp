#include "ui/DataInspector.h"

#include "core/FileSource.h"
#include "core/FileType.h"
#include "ui/Desktop.h"

#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QGuiApplication>
#include <QHeaderView>
#include <QTimeZone>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtEndian>

#include <bit>
#include <cstring>
#include <optional>
#include <span>

namespace aiedit::ui {

namespace {

enum Row
{
    BinaryRow,
    UInt8Row,
    Int8Row,
    UInt16Row,
    Int16Row,
    UInt32Row,
    Int32Row,
    UInt64Row,
    Int64Row,
    Float32Row,
    Float64Row,
    UnixTimeRow,
    Utf8Row,
    RowCount,
};

template <typename T>
std::optional<T> read(std::span<const std::uint8_t> bytes, std::uint64_t offset, bool bigEndian)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
        return std::nullopt;
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return bigEndian ? qFromBigEndian(value) : qFromLittleEndian(value);
}

// std::optional::transform is C++23.
template <typename T, typename F>
std::optional<QString> map(const std::optional<T>& value, F format)
{
    return value ? std::optional<QString>(format(*value)) : std::nullopt;
}

// The code point starting at offset, if the bytes there form exactly one valid UTF-8 character.
QString utf8At(std::span<const std::uint8_t> bytes, std::uint64_t offset)
{
    if (offset >= bytes.size())
        return {};
    const std::uint8_t lead = bytes[offset];
    const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
    if (length == 0 || bytes.size() - offset < length)
        return {};
    const auto sequence = bytes.subspan(offset, length);
    if (!core::isValidUtf8(sequence))
        return {};

    const QString character =
        QString::fromUtf8(reinterpret_cast<const char*>(sequence.data()), static_cast<qsizetype>(length));
    const char32_t codePoint = character.toUcs4().value(0);
    const QString hex = QStringLiteral("U+%1").arg(static_cast<uint>(codePoint), 4, 16, QLatin1Char('0')).toUpper();
    if (codePoint < 0x20 || codePoint == 0x7F)
        return hex; // control characters have no useful glyph
    return QStringLiteral("%1  %2").arg(character, hex);
}

} // namespace

DataInspector::DataInspector(QWidget* parent) : QWidget(parent)
{
    m_endianness = new QComboBox(this);
    m_endianness->addItem(tr("Little-endian"));
    m_endianness->addItem(tr("Big-endian"));
    connect(m_endianness, &QComboBox::currentIndexChanged, this, &DataInspector::refresh);

    m_values = new QTreeWidget(this);
    m_values->setColumnCount(2);
    m_values->setHeaderLabels({tr("Type"), tr("Value")});
    m_values->setRootIsDecorated(false);
    m_values->setUniformRowHeights(true);
    m_values->setAlternatingRowColors(true);
    m_values->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_values->header()->setStretchLastSection(true);

    const QString labels[RowCount] = {
        tr("Binary"), tr("UInt8"),   tr("Int8"),    tr("UInt16"),  tr("Int16"),
        tr("UInt32"), tr("Int32"),   tr("UInt64"),  tr("Int64"),   tr("Float32"),
        tr("Float64"), tr("Unix time (32-bit)"), tr("UTF-8"),
    };
    const QFont fixed = fixedFont();
    for (const QString& label : labels) {
        auto* item = new QTreeWidgetItem(m_values, {label, QString()});
        item->setFont(1, fixed);
    }

    auto* copyValue = new QAction(tr("Copy Value"), m_values);
    copyValue->setShortcut(QKeySequence::Copy);
    copyValue->setShortcutContext(Qt::WidgetShortcut);
    connect(copyValue, &QAction::triggered, this, [this] {
        if (const QTreeWidgetItem* item = m_values->currentItem(); item && !item->text(1).isEmpty())
            QGuiApplication::clipboard()->setText(item->text(1));
    });
    m_values->addAction(copyValue);
    m_values->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_endianness);
    layout->addWidget(m_values);

    refresh();
}

DataInspector::~DataInspector() = default;

void DataInspector::setSource(std::shared_ptr<const core::FileSource> source)
{
    m_source = std::move(source);
    m_offset = 0;
    refresh();
}

void DataInspector::setOffset(std::uint64_t offset)
{
    m_offset = offset;
    refresh();
}

bool DataInspector::isBigEndian() const
{
    return m_endianness->currentIndex() == 1;
}

void DataInspector::setBigEndian(bool bigEndian)
{
    m_endianness->setCurrentIndex(bigEndian ? 1 : 0);
}

QString DataInspector::valueText(const QString& label) const
{
    const auto items = m_values->findItems(label, Qt::MatchExactly, 0);
    return items.isEmpty() ? QString() : items.first()->text(1);
}

void DataInspector::refresh()
{
    const std::span<const std::uint8_t> bytes = m_source ? m_source->bytes() : std::span<const std::uint8_t>{};
    const bool big = isBigEndian();
    const QString none = QStringLiteral("—");

    auto show = [&](int row, const std::optional<QString>& text) {
        m_values->topLevelItem(row)->setText(1, text.value_or(none));
    };
    auto number = [](auto value) { return QString::number(value); };

    const auto u8 = read<std::uint8_t>(bytes, m_offset, big);
    const auto u16 = read<std::uint16_t>(bytes, m_offset, big);
    const auto u32 = read<std::uint32_t>(bytes, m_offset, big);
    const auto u64 = read<std::uint64_t>(bytes, m_offset, big);

    show(BinaryRow, map(u8, [](std::uint8_t v) { return QStringLiteral("%1").arg(v, 8, 2, QLatin1Char('0')); }));
    show(UInt8Row, map(u8, number));
    show(Int8Row, map(u8, [&](std::uint8_t v) { return number(static_cast<std::int8_t>(v)); }));
    show(UInt16Row, map(u16, number));
    show(Int16Row, map(u16, [&](std::uint16_t v) { return number(static_cast<std::int16_t>(v)); }));
    show(UInt32Row, map(u32, number));
    show(Int32Row, map(u32, [&](std::uint32_t v) { return number(static_cast<std::int32_t>(v)); }));
    show(UInt64Row, map(u64, [](std::uint64_t v) { return QString::number(static_cast<qulonglong>(v)); }));
    show(Int64Row, map(u64, [](std::uint64_t v) { return QString::number(static_cast<qlonglong>(v)); }));
    show(Float32Row, map(u32, [](std::uint32_t v) { return QString::number(std::bit_cast<float>(v), 'g', 9); }));
    show(Float64Row, map(u64, [](std::uint64_t v) { return QString::number(std::bit_cast<double>(v), 'g', 17); }));
    show(UnixTimeRow, map(u32, [](std::uint32_t v) {
        return QDateTime::fromSecsSinceEpoch(v, QTimeZone::UTC).toString(Qt::ISODate);
    }));

    const QString utf8 = utf8At(bytes, m_offset);
    show(Utf8Row, utf8.isEmpty() ? std::nullopt : std::optional<QString>(utf8));
}

} // namespace aiedit::ui
