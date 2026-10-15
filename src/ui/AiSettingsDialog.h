#pragma once

#include "ui/AiSettings.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace aiedit::ui {

class AiSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AiSettingsDialog(const AiSettings& settings, QWidget* parent = nullptr);

    AiSettings settings() const;

private:
    void providerChanged();

    AiSettings m_original;
    QComboBox* m_provider = nullptr;
    QLineEdit* m_baseUrl = nullptr;
    QComboBox* m_model = nullptr;
    QLineEdit* m_key = nullptr;
    QCheckBox* m_showKey = nullptr;
    QCheckBox* m_thinking = nullptr;
    QLabel* m_keyNote = nullptr;
};

} // namespace aiedit::ui
