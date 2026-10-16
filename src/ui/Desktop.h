#pragma once

#include <QString>

namespace aiedit::ui {

// Shows a file or folder in the system's file manager: selected in Explorer and Finder; on
// Linux, where desktops have no common way to select it, its folder opens.
void showInFileManager(const QString& path);

} // namespace aiedit::ui
