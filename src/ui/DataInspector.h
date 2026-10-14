#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>

class QComboBox;
class QTreeWidget;

namespace cppedit::core {
class FileSource;
}

namespace cppedit::ui {

// Shows the bytes at one offset decoded as integers, floats, a timestamp and UTF-8.
class DataInspector : public QWidget
{
    Q_OBJECT

public:
    explicit DataInspector(QWidget* parent = nullptr);
    ~DataInspector() override;

    void setSource(std::shared_ptr<const core::FileSource> source);
    void setOffset(std::uint64_t offset);

    bool isBigEndian() const;
    void setBigEndian(bool bigEndian);

    // The value text of the row with the given label, for tests.
    QString valueText(const QString& label) const;

private:
    void refresh();

    QComboBox* m_endianness = nullptr;
    QTreeWidget* m_values = nullptr;
    std::shared_ptr<const core::FileSource> m_source;
    std::uint64_t m_offset = 0;
};

} // namespace cppedit::ui
