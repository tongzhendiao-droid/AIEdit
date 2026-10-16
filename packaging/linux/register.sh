#!/usr/bin/env bash
# Registers an installed AIEdit with the desktop, for the current user only:
#   - refreshes the application and icon caches, so menus and "Open with" know it;
#   - adds "Open with AIEdit" to the Nautilus right-click menu (under Scripts), for any file or
#     folder (a folder opens in the folder panel);
#   - with --default, makes AIEdit the default application for plain text, logs, data files and
#     binaries. File types you have pointed at another program yourself (C/C++ sources, shell
#     scripts, CSV, HTML) are left alone: AIEdit cannot edit yet.
#
# Usage: packaging/linux/register.sh [--default] [--prefix DIR]   (DIR defaults to ~/.local)
set -euo pipefail

prefix="$HOME/.local"
set_default=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --default) set_default=1 ;;
        --prefix) prefix="$2"; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

binary="$prefix/bin/aiedit"
data="$prefix/share"
if [[ ! -x "$binary" || ! -f "$data/applications/aiedit.desktop" ]]; then
    echo "AIEdit is not installed under $prefix; run: cmake --install <build dir>" >&2
    exit 1
fi

update-desktop-database "$data/applications" 2>/dev/null || true
gtk-update-icon-cache --force --ignore-theme-index "$data/icons/hicolor" >/dev/null 2>&1 || true

# Nautilus runs these with the selected paths in NAUTILUS_SCRIPT_SELECTED_FILE_PATHS, one per line.
scripts="${XDG_DATA_HOME:-$HOME/.local/share}/nautilus/scripts"
case "${LANG:-}" in
    zh*) script_name="用 AIEdit 打开" ;;
    *) script_name="Open with AIEdit" ;;
esac
mkdir -p "$scripts"
cat >"$scripts/$script_name" <<EOF
#!/bin/sh
# Opens the files and folders selected in Nautilus in AIEdit (installed by AIEdit's register.sh).
IFS='
'
exec "$binary" \$NAUTILUS_SCRIPT_SELECTED_FILE_PATHS
EOF
chmod +x "$scripts/$script_name"
echo "Right-click menu: Scripts → $script_name"

if [[ $set_default == 1 ]]; then
    types=(
        text/plain application/x-zerosize text/markdown text/x-log
        application/json application/xml text/xml application/x-yaml application/toml
        text/x-python text/x-python3 text/x-cmake text/x-patch text/x-diff
        application/octet-stream application/x-executable application/x-pie-executable
        application/x-sharedlib application/x-object application/x-core
    )
    for type in "${types[@]}"; do
        xdg-mime default aiedit.desktop "$type"
    done
    echo "AIEdit is now the default for ${#types[@]} file types (see ~/.config/mimeapps.list)."
    echo "To hand plain text back to GNOME Text Editor:"
    echo "  xdg-mime default org.gnome.TextEditor.desktop text/plain"
fi
