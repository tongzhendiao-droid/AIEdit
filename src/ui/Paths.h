#pragma once

#include <QFile>
#include <QString>

#include <filesystem>

namespace aiedit::ui {

// QString <-> std::filesystem::path without assuming UTF-8 file names on POSIX:
// QFile::encodeName/decodeName use the same conversion as the rest of Qt.
inline std::filesystem::path toPath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).toStdString());
#endif
}

inline QString toQString(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.native());
#else
    return QFile::decodeName(QByteArray::fromStdString(path.native()));
#endif
}

} // namespace aiedit::ui
