#pragma once

#include <QStringView>

#include <optional>
#include <vector>

namespace aiedit::ui {

// How well a query matches a text the way Ctrl+P matches names in editors: the query's
// characters in order, not necessarily together, ignoring case, so "mwc" finds MainWindow.cpp.
// Characters that match side by side score more, and so do those at the start of a word (of
// the text, after / _ - . or a space, or where lower case turns upper); each character skipped
// between two matches costs a little. Nothing if the characters are not all there, in order.
//
// With `positions`, also where the characters matched (ascending), to show them in bold.
std::optional<int> fuzzyMatch(QStringView query, QStringView text, std::vector<int>* positions = nullptr);

} // namespace aiedit::ui
