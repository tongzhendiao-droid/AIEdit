#include "ui/Desktop.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QLocale>
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

QFont withCjkFallback(QFont font)
{
    static const QStringList fallback = [] {
        const QStringList simplified{
            QStringLiteral("Noto Sans CJK SC"),   QStringLiteral("Source Han Sans SC"),
            QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
            QStringLiteral("PingFang SC"),        QStringLiteral("Noto Sans SC"),
            QStringLiteral("WenQuanYi Micro Hei")};
        const QStringList traditional{
            QStringLiteral("Noto Sans CJK TC"),      QStringLiteral("Source Han Sans TC"),
            QStringLiteral("Microsoft JhengHei UI"), QStringLiteral("Microsoft JhengHei"),
            QStringLiteral("PingFang TC"),           QStringLiteral("Noto Sans TC")};
        const QStringList japanese{QStringLiteral("Noto Sans CJK JP"), QStringLiteral("Source Han Sans JP"),
                                   QStringLiteral("Yu Gothic UI"),     QStringLiteral("Meiryo UI"),
                                   QStringLiteral("Hiragino Sans"),    QStringLiteral("Noto Sans JP")};
        const QStringList korean{QStringLiteral("Noto Sans CJK KR"), QStringLiteral("Source Han Sans KR"),
                                 QStringLiteral("Malgun Gothic"),    QStringLiteral("Apple SD Gothic Neo"),
                                 QStringLiteral("Noto Sans KR")};
        const QLocale locale;
        QList<QStringList> order;
        if (locale.language() == QLocale::Japanese)
            order = {japanese, simplified, traditional, korean};
        else if (locale.language() == QLocale::Korean)
            order = {korean, simplified, traditional, japanese};
        else if (locale.script() == QLocale::TraditionalChineseScript)
            order = {traditional, simplified, japanese, korean};
        else
            order = {simplified, traditional, japanese, korean};
        for (const QStringList& families : std::as_const(order)) {
            for (const QString& family : families) {
                if (QFontDatabase::hasFamily(family))
                    return QStringList{family};
            }
        }
        return QStringList();
    }();
    // An alias such as Monospace is no family of its own: with another family after it, Qt would
    // take that one instead. The family it stands for, then.
    QStringList families{QFontInfo(font).family()};
    for (const QString& family : font.families()) {
        if (!families.contains(family) && QFontDatabase::hasFamily(family))
            families << family;
    }
    for (const QString& family : fallback) {
        if (!families.contains(family))
            families << family;
    }
    font.setFamilies(families);
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
