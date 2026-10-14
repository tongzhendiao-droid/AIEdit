#pragma once

#include "core/FileType.h"

#include <QWidget>

#include <memory>

namespace cppedit::core {
class FileSource;
}

namespace cppedit::ui {

class HexView;

// One open file: its bytes, what kind of file it is, and the views showing it.
// Only the hex view exists so far; the text view joins it in M2.
class DocumentView : public QWidget
{
    Q_OBJECT

public:
    DocumentView(std::shared_ptr<const core::FileSource> source, const core::FileTypeInfo& fileType,
                 QWidget* parent = nullptr);
    ~DocumentView() override;

    const std::shared_ptr<const core::FileSource>& source() const { return m_source; }
    const core::FileTypeInfo& fileType() const { return m_fileType; }
    QString filePath() const;
    HexView* hexView() const { return m_hexView; }

private:
    std::shared_ptr<const core::FileSource> m_source;
    core::FileTypeInfo m_fileType;
    HexView* m_hexView = nullptr;
};

} // namespace cppedit::ui
