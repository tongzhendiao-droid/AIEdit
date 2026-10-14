#include "ui/DocumentView.h"

#include "core/FileSource.h"
#include "ui/HexView.h"
#include "ui/Paths.h"

#include <QVBoxLayout>

namespace cppedit::ui {

DocumentView::DocumentView(std::shared_ptr<const core::FileSource> source, const core::FileTypeInfo& fileType,
                           QWidget* parent)
    : QWidget(parent), m_source(std::move(source)), m_fileType(fileType)
{
    m_hexView = new HexView(this);
    m_hexView->setFrameShape(QFrame::NoFrame);
    m_hexView->setSource(m_source);
    setFocusProxy(m_hexView);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_hexView);
}

DocumentView::~DocumentView() = default;

QString DocumentView::filePath() const
{
    return toQString(m_source->path());
}

} // namespace cppedit::ui
