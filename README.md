# cppEdit

快速、带 AI 辅助的文本与二进制文件查看器。用 C++20 和 Qt 6 编写，以查看为主、编辑为辅。

> 当前进度：**M1**，即 Hex 查看器。文本视图、AI 面板等后续计划见 [DESIGN.md](DESIGN.md)。

## 功能（M1）

- **秒开任意大小的文件**：用 mmap 映射文件，只绘制屏幕上可见的行，TB 级文件也能流畅滚动。
- **Hex 视图**：偏移、十六进制、字符三栏，按字节类别着色（NUL、空白、控制字符、高位字节）。
- **选择和复制**：用键盘或鼠标选择，可复制为十六进制、文本（合法 UTF-8 会按原文复制）、C 数组或偏移。
- **跳转**：`Ctrl+G` 跳转到偏移，支持 `0x1F`、`1Fh`、`31` 以及 `+16`、`-0x10` 这类相对偏移。
- **数据检查器**：把光标处的字节解析为各种整数、浮点数、Unix 时间和 UTF-8 字符，可切换大小端。
- **文件类型识别**：识别文本还是二进制、编码（ASCII、UTF-8、UTF-16/32、GBK 等非 UTF-8 编码）以及约 50 种二进制格式（ELF、PE、PNG、ZIP、SQLite……）。
- **其他**：多标签页，拖放打开文件，缩放（`Ctrl+滚轮`），中文界面。

| 操作 | 按键 |
|---|---|
| 移动光标 | 方向键、`PgUp`/`PgDn`、`Home`/`End`、`Ctrl+Home`/`Ctrl+End` |
| 扩展选区 | 按住 `Shift` 移动光标，或用鼠标拖动 |
| 切换 Hex 栏和字符栏 | `Tab` |
| 复制（格式取决于当前所在的栏） | `Ctrl+C` |
| 跳转到偏移 | `Ctrl+G` |
| 缩放 | `Ctrl++`、`Ctrl+-`、`Ctrl+0` |

## 构建

依赖：CMake 3.21 或更高、支持 C++20 的编译器，以及 Qt 6.5 或更高（Core、Gui、Widgets、Test）。编译界面翻译还需要 Qt 的 `lrelease`。

**Ubuntu / Debian**

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-l10n-tools
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j
```

```bash
./build/src/cppedit 文件1 文件2
```

**Windows**：安装 Qt 6（MSVC 版本）和 CMake 后，用相同的 CMake 命令构建。Windows 版目前还没有验证过。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

GUI 测试以 offscreen 方式运行，不会弹出窗口。如果设置了 `CPPEDIT_SCREENSHOT_DIR`，测试会把截图保存到这个目录。

## 翻译

界面字符串在源码中用英文书写并包上 `tr()`。修改界面文字后，运行下面的命令更新 `translations/*.ts`，再补全译文（可以用 Qt Linguist）：

```bash
cmake --build build --target update_translations
```

## 许可证

[GPL-3.0-or-later](LICENSE)
