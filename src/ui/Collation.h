#pragma once

#include <QCollator>
#include <QLocale>

namespace aiedit::ui {

// A collator for the user's language that orders numbers by value ("2" before "10"). Without
// a language set (the C locale, common in containers and minimal systems), Qt compares code
// points and ignores numbers, so English stands in for it.
inline QCollator naturalCollator()
{
    QLocale locale;
    if (locale.language() == QLocale::C)
        locale = QLocale(QLocale::English, QLocale::UnitedStates);
    QCollator collator(locale);
    collator.setNumericMode(true);
    return collator;
}

} // namespace aiedit::ui
