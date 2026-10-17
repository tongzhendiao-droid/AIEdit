#pragma once

#include <QFont>
#include <QString>

#include <string_view>

namespace aiedit::ui {

// Shows a file or folder in the system's file manager: selected in Explorer and Finder; on
// Linux, where desktops have no common way to select it, its folder opens.
void showInFileManager(const QString& path);

// The monospaced font for text and bytes: the system's, except on Windows, whose own is Courier
// New; there it is the one its editors use (Cascadia Mono, or Consolas), at 10 points at least.
QFont fixedFont();

// The codec Windows writes text in for programs that are not Unicode, whatever the language of
// its menus: GB18030 for code page 936, Big5 for 950 and so on. Empty elsewhere, and where that
// code page is UTF-8 or one AIEdit lacks.
std::string_view systemLegacyCodec();

// Lets another program bring its window to the front, as the window already open does when a
// second launch hands it files. Windows lets only the program the user just started do that
// (elsewhere there is nothing to allow).
void allowActivationByOthers();

} // namespace aiedit::ui
