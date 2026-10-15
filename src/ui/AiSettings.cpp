#include "ui/AiSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace aiedit::ui {

namespace {

QString environmentKey(AiSettings::Provider provider)
{
    QString key = qEnvironmentVariable(AiSettings::kKeyEnvironmentVariable);
    if (key.isEmpty() && provider == AiSettings::Provider::DeepSeek)
        key = qEnvironmentVariable(AiSettings::kDeepSeekKeyEnvironmentVariable);
    return key.trimmed();
}

} // namespace

QString AiSettings::keyFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/api-key");
}

AiSettings AiSettings::load()
{
    AiSettings result;
    QSettings settings;
    settings.beginGroup(QStringLiteral("ai"));
    result.provider = settings.value(QStringLiteral("provider")).toString() == QStringLiteral("openai")
                          ? Provider::OpenAiCompatible
                          : Provider::DeepSeek;
    result.baseUrl = settings.value(QStringLiteral("baseUrl"), result.baseUrl).toString();
    result.model = settings.value(QStringLiteral("model"), result.model).toString();
    result.thinking = settings.value(QStringLiteral("thinking"), false).toBool();
    settings.endGroup();

    result.apiKey = environmentKey(result.provider);
    result.keyFromEnvironment = !result.apiKey.isEmpty();
    if (!result.keyFromEnvironment) {
        QFile file(keyFilePath());
        if (file.open(QIODevice::ReadOnly))
            result.apiKey = QString::fromUtf8(file.readAll()).trimmed();
    }
    return result;
}

void AiSettings::save() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("ai"));
    settings.setValue(QStringLiteral("provider"),
                      provider == Provider::OpenAiCompatible ? QStringLiteral("openai") : QStringLiteral("deepseek"));
    settings.setValue(QStringLiteral("baseUrl"), baseUrl);
    settings.setValue(QStringLiteral("model"), model);
    settings.setValue(QStringLiteral("thinking"), thinking);
    settings.endGroup();

    if (keyFromEnvironment)
        return;
    const QString path = keyFilePath();
    if (apiKey.isEmpty()) {
        QFile::remove(path);
        return;
    }
    // Created readable by the owner only, so the key is never visible to others, not even briefly.
    QDir().mkpath(QFileInfo(path).absolutePath());
    const auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QFile file(path);
    if (file.exists())
        file.setPermissions(ownerOnly);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate, ownerOnly))
        file.write(apiKey.toUtf8() + '\n');
}

QUrl AiSettings::chatUrl() const
{
    QString url = baseUrl.trimmed();
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    return QUrl(url + QStringLiteral("/chat/completions"));
}

} // namespace aiedit::ui
