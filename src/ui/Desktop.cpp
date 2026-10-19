#include "ui/Desktop.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QProcess>
#include <QUrl>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace aiedit::ui {

void showInFileManager(const QString& path)
{
    if (path.isEmpty())
        return;
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    const QFileInfo info(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? path : info.absolutePath()));
#endif
}

QFont fixedFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#ifdef Q_OS_WIN
    for (const QString& family : {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas")}) {
        if (QFontDatabase::hasFamily(family)) {
            font.setFamily(family);
            break;
        }
    }
    font.setPointSize(std::max(font.pointSize(), 10));
#endif
    return font;
}

std::string_view systemLegacyCodec()
{
#ifdef Q_OS_WIN
    switch (::GetACP()) {
    case 936:
        return "GB18030";
    case 950:
        return "Big5";
    case 932:
        return "Shift_JIS";
    case 949:
        return "EUC-KR";
    case 1250:
        return "windows-1250";
    case 1251:
        return "windows-1251";
    case 1252:
        return "windows-1252";
    case 1253:
        return "windows-1253";
    case 1254:
        return "windows-1254";
    case 1256:
        return "windows-1256";
    default:
        break;
    }
#endif
    return {};
}

void allowActivationByOthers()
{
#ifdef Q_OS_WIN
    ::AllowSetForegroundWindow(ASFW_ANY);
#endif
}

} // namespace aiedit::ui
