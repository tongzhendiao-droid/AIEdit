#pragma once

#include "core/FileType.h"
#include "core/LineIndex.h"
#include "core/TextCodec.h"
#include "ui/ChatClient.h"
#include "ui/FieldMark.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <atomic>
#include <functional>
#include <memory>

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

// The open document as the assistant's tools see it. Everything is shared, so a tool can keep
// working while the user switches tabs or the file reloads.
struct ToolTarget
{
    QString fileName;
    std::shared_ptr<const core::FileSource> source;
    std::shared_ptr<const core::LineIndex> index;
    core::TextCodec codec = core::utf8Codec();
    core::FileTypeInfo type;
    std::function<void(const QList<FieldMark>&)> markFields; // shows marks in the hex view
};

// Tools the assistant can call. They run here, on the open file, so the file never has to be
// sent whole: the model reads, searches and marks what it needs, and every result is text
// capped at a modest size.
namespace tools {

QJsonArray definitions();

QString fileInfo(const ToolTarget& target);
QString readBytes(const ToolTarget& target, const QJsonObject& arguments);
QString readLines(const ToolTarget& target, const QJsonObject& arguments);
QString search(const ToolTarget& target, const QJsonObject& arguments, const std::atomic<bool>& cancelled);
QString markFields(const ToolTarget& target, const QJsonObject& arguments);

} // namespace tools

// Runs tool calls one at a time; searches go to a worker thread so the window stays responsive.
class ToolRunner : public QObject
{
    Q_OBJECT

public:
    explicit ToolRunner(QObject* parent = nullptr);
    ~ToolRunner() override;

    // `done` receives the result text on this thread, unless cancel() comes first.
    void run(const ToolCall& call, const ToolTarget& target, std::function<void(const QString&)> done);
    void cancel();
    bool isRunning() const { return m_watcher.isRunning(); }

private:
    QFutureWatcher<QString> m_watcher;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    std::function<void(const QString&)> m_done;
};

} // namespace aiedit::ui
