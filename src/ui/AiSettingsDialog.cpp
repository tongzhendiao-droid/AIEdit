#include "ui/AiSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace aiedit::ui {

namespace {

const QString kDeepSeekUrl = QStringLiteral("https://api.deepseek.com");
const QStringList kDeepSeekModels{QStringLiteral("deepseek-flash"), QStringLiteral("deepseek-v4-pro")};

} // namespace

AiSettingsDialog::AiSettingsDialog(const AiSettings& settings, QWidget* parent)
    : QDialog(parent), m_original(settings)
{
    setWindowTitle(tr("AI Settings"));

    m_provider = new QComboBox(this);
    m_provider->addItem(QStringLiteral("DeepSeek"), static_cast<int>(AiSettings::Provider::DeepSeek));
    m_provider->addItem(tr("Other (OpenAI-compatible)"), static_cast<int>(AiSettings::Provider::OpenAiCompatible));
    m_provider->setCurrentIndex(m_provider->findData(static_cast<int>(settings.provider)));

    m_baseUrl = new QLineEdit(settings.baseUrl, this);
    m_baseUrl->setPlaceholderText(QStringLiteral("https://…/v1"));
    m_model = new QComboBox(this);
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);

    m_key = new QLineEdit(this);
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(QStringLiteral("sk-…"));
    m_showKey = new QCheckBox(tr("Show"), this);
    connect(m_showKey, &QCheckBox::toggled, this,
            [this](bool show) { m_key->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password); });
    m_keyNote = new QLabel(this);
    m_keyNote->setWordWrap(true);
    m_keyNote->setForegroundRole(QPalette::PlaceholderText);
    if (settings.keyFromEnvironment) {
        // The environment wins; editing the field here would have no effect.
        m_key->setEnabled(false);
        m_key->setPlaceholderText(tr("(from the environment)"));
        m_showKey->setEnabled(false);
        m_keyNote->setText(tr("The key comes from the %1 environment variable and is not saved.")
                               .arg(QString::fromLatin1(AiSettings::kKeyEnvironmentVariable)));
    } else {
        m_key->setText(settings.apiKey);
        m_keyNote->setText(tr("Saved only on this computer, in %1, readable by you alone. "
                              "The %2 environment variable can be used instead.")
                               .arg(QDir::toNativeSeparators(AiSettings::keyFilePath()),
                                    QString::fromLatin1(AiSettings::kKeyEnvironmentVariable)));
    }

    m_thinking = new QCheckBox(tr("Deep thinking (slower; for hard questions)"), this);
    m_thinking->setChecked(settings.thinking);

    auto* keyRow = new QHBoxLayout;
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(m_showKey);

    auto* form = new QFormLayout;
    form->addRow(tr("Service:"), m_provider);
    form->addRow(tr("Address:"), m_baseUrl);
    form->addRow(tr("Model:"), m_model);
    form->addRow(tr("API key:"), keyRow);
    form->addRow(QString(), m_keyNote);
    form->addRow(QString(), m_thinking);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(m_provider, &QComboBox::currentIndexChanged, this, &AiSettingsDialog::providerChanged);
    providerChanged();
    m_model->setCurrentText(settings.model);
    resize(560, sizeHint().height());
}

void AiSettingsDialog::providerChanged()
{
    const bool deepSeek = m_provider->currentData().toInt() == static_cast<int>(AiSettings::Provider::DeepSeek);
    const QString model = m_model->currentText();
    m_model->clear();
    if (deepSeek) {
        m_model->addItems(kDeepSeekModels);
        if (m_baseUrl->text().trimmed().isEmpty())
            m_baseUrl->setText(kDeepSeekUrl);
    } else if (m_baseUrl->text().trimmed() == kDeepSeekUrl) {
        m_baseUrl->clear(); // another service needs its own address
    }
    m_model->setCurrentText(deepSeek && !kDeepSeekModels.contains(model) ? kDeepSeekModels.first() : model);
    m_thinking->setVisible(deepSeek); // only DeepSeek has the switch
}

AiSettings AiSettingsDialog::settings() const
{
    AiSettings result = m_original;
    result.provider = static_cast<AiSettings::Provider>(m_provider->currentData().toInt());
    result.baseUrl = m_baseUrl->text().trimmed();
    result.model = m_model->currentText().trimmed();
    if (!result.keyFromEnvironment)
        result.apiKey = m_key->text().trimmed();
    result.thinking = result.provider == AiSettings::Provider::DeepSeek && m_thinking->isChecked();
    return result;
}

} // namespace aiedit::ui
