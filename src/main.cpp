#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("AIEdit"));
    QApplication::setApplicationName(QStringLiteral("AIEdit"));
    QApplication::setApplicationVersion(QStringLiteral(AIEDIT_VERSION));

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
    parser.addPositionalArgument(QStringLiteral("files"), QApplication::translate("main", "Files to open."),
                                 QStringLiteral("[files...]"));
    parser.process(app);

    aiedit::ui::MainWindow window;
    window.show();
    for (const QString& path : parser.positionalArguments())
        window.openFile(path);

    return QApplication::exec();
}
