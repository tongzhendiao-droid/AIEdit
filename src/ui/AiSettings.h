#pragma once

#include <QString>
#include <QUrl>

namespace aiedit::ui {

// Where AI requests go. Everything but the key is ordinary QSettings; the key is kept in a
// separate file only the user can read (mode 0600), or comes from the environment and is then
// never saved.
struct AiSettings
{
    enum class Provider
    {
        DeepSeek,
        OpenAiCompatible, // any /chat/completions service: Qwen, Kimi, Ollama, vLLM, …
    };

    static constexpr const char* kKeyEnvironmentVariable = "AIEDIT_API_KEY";
    static constexpr const char* kDeepSeekKeyEnvironmentVariable = "DEEPSEEK_API_KEY";

    Provider provider = Provider::DeepSeek;
    QString baseUrl = QStringLiteral("https://api.deepseek.com");
    QString model = QStringLiteral("deepseek-flash");
    QString apiKey;
    bool keyFromEnvironment = false;
    // DeepSeek's thinking mode: slower, better for hard questions. Off for quick actions.
    bool thinking = false;

    static AiSettings load();
    // Saves everything; the key only if it did not come from the environment.
    void save() const;
    // Where save() puts the key.
    static QString keyFilePath();

    QUrl chatUrl() const;
    bool isConfigured() const { return !apiKey.isEmpty() && !baseUrl.isEmpty() && !model.isEmpty(); }
};

} // namespace aiedit::ui
