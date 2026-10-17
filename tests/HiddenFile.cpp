#include "HiddenFile.h"

#include <QDir>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#endif

bool markHidden(const QString& path)
{
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = ::GetFileAttributesW(native.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           ::SetFileAttributesW(native.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN) != 0;
#else
    return path.section(QLatin1Char('/'), -1).startsWith(QLatin1Char('.'));
#endif
}
