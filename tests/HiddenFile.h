#pragma once

#include <QString>

// Hides a file or folder as the system does: on Unix its name starts with a dot, which the test
// gave it already; on Windows it also takes the hidden attribute, as a dot means nothing there.
bool markHidden(const QString& path);
