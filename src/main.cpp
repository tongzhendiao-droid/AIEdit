#include "ui/MainWindow.h"
#include "ui/SingleInstance.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("AIEdit"));
    QApplication::setApplicationName(QStringLiteral("AIEdit"));
    QApplication::setApplicationVersion(QStringLiteral(AIEDIT_VERSION));
    // Matches aiedit.desktop, so the desktop shows the right name and icon (Wayland app id).
    QApplication::setDesktopFileName(QStringLiteral("aiedit"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("aiedit"), QIcon(QStringLiteral(":/icons/aiedit.svg"))));

    // Qt's own strings (standard buttons, file dialogs), then ours. Both are optional.
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);
    QTranslator appTranslator;
    if (appTranslator.load(QLocale(), QStringLiteral("aiedit"), QStringLiteral("_"), QStringLiteral(":/i18n")))
        QApplication::installTranslator(&appTranslator);

    QCommandLineParser parser;
    parser.setApplicationDescription(QApplication::translate("main", "A fast viewer for text and binary files."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption newWindow(QStringLiteral("new-window"),
                                       QApplication::translate("main", "Open a separate window instead of using "
                                                                       "the one already open."));
    parser.addOption(newWindow);
    parser.addPositionalArgument(QStringLiteral("files"), QApplication::translate("main", "Files to open."),
                                 QStringLiteral("[files...]"));
    parser.process(app);

    // The running instance has its own working directory, so send it absolute paths.
    QStringList files;
    for (const QString& path : parser.positionalArguments())
        files << QFileInfo(path).absoluteFilePath();

    aiedit::ui::SingleInstance instance;
    if (!parser.isSet(newWindow)) {
        if (instance.forward(files))
            return 0;
        instance.listen();
    }

    aiedit::ui::MainWindow window;
    QObject::connect(&instance, &aiedit::ui::SingleInstance::filesReceived, &window, [&window](const QStringList& paths) {
        for (const QString& path : paths)
            window.openFile(path);
        window.setWindowState((window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        window.raise();
        window.activateWindow();
    });
    window.show();
    for (const QString& path : files)
        window.openFile(path);

    return QApplication::exec();
}
