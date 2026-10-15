# AIEdit

[![CI](https://github.com/tongzhendiao-droid/AIEdit/actions/workflows/ci.yml/badge.svg)](https://github.com/tongzhendiao-droid/AIEdit/actions/workflows/ci.yml)

快速、带 AI 辅助的文本与二进制文件查看器。用 C++20 和 Qt 6 编写，以查看为主、编辑为辅。

> 当前进度：已完成 **M6b**（未保存内容自动备份、行操作、配色方案、在文件夹里查找）。之前的 M6 是日志过滤：只看想看的行，可以用一句话让 AI 写规则，规则在本机对整个文件执行。计划见 [DESIGN.md](DESIGN.md)。

## 功能

**打开任意大小的文件**
- 用 mmap 映射文件，只绘制屏幕上可见的行，TB 级文件也能流畅滚动。
- 大文件在后台建行索引，第一屏马上就能显示。
- 超长行（比如压缩过的 JSON）会自动拆成多行显示，不会卡住。

**文本视图**
- 中日韩文字、emoji、Tab 都能正确显示；控制字符显示为 ␀、␛ 这类可见符号。
- 长行默认按窗口宽度自动换行，看小说、长段落不用横向滚动；`Alt+Z` 可以关掉，改为横向滚动。
- 自动识别编码（UTF-8/16/32、GB18030、Big5、Shift_JIS 等），也可以在状态栏一键切换（20 种）。
- 语法高亮支持 300 多种语言（借助 KDE 的 KSyntaxHighlighting），可在状态栏切换语言。
- 配色方案（视图 →「配色方案」）：默认跟随系统，也可以选 Monokai、Dracula、Solarized、GitHub、Nord、Tokyo Night 等 29 套主题，编辑区、十六进制视图和整个界面一起变。编辑区字体也可以换（视图 →「编辑区字体」）。
- 显示行号；`Ctrl+G` 转到行号，也可以输入 `0x1F` 这样的偏移。

**十六进制视图**
- 偏移、十六进制、字符三栏，按字节类别着色。
- 可复制为十六进制、文本、C 数组或偏移。
- 数据检查器：把光标处的字节解析为各种整数、浮点数、Unix 时间和 UTF-8 字符，可切换大小端。

**联动和搜索**
- 文本、十六进制、并排三种模式，光标和选区始终同步。
- 查找支持三种模式：
  - 文本：可选区分大小写。
  - 正则表达式。
  - 十六进制：用 `??` 表示任意字节，如 `4D 5A ?? 00`。
- 搜索在后台运行，大文件也不会卡住界面。搜索 GBK 文件时，会跳过落在字符中间的假匹配。
- 替换（`Ctrl+H`）：逐个替换，或全部替换（一步就能撤销）。正则模式下用 `$1` 或 `\1` 引用分组，`\n` 插入换行（自动用文件原来的换行符）。

**编辑**（16 MiB 以内的文本文件）
- 直接输入，支持中文输入法；撤销、重做、剪切、粘贴，选中多行时 `Tab` / `Shift+Tab` 整体缩进。
- 回车沿用上一行的缩进；`Tab` 跟随文件本身的风格（制表符，或 2、4 个空格）。
- 保存（`Ctrl+S`）时保持文件原来的编码、BOM 和换行符；先写临时文件再整体替换，保存一半出错也不会损坏原文件。
- 有未保存的修改时，标签页标题前显示 `*`；关闭标签页或窗口前会询问是否保存。
- 新建文档：`Ctrl+N`、标签栏右侧的 `+`，或双击标签栏空白处；第一次保存时选择保存位置。
- 转换编码：状态栏的编码按钮 →「以其他编码保存」，比如把 GBK 文件存成 UTF-8；有无法识别的字节、或新编码表示不了的字符时会先告诉你。
- 统一换行符：点状态栏的换行符（LF / CRLF），可以把所有行转换为 LF、CRLF 或 CR，一步就能撤销。
- 行操作（编辑 →「行操作」）：排序（数字按大小）、删除重复行、删除空行、删除行尾空白，上移、下移行（`Alt+↑` / `Alt+↓`），复制行（`Ctrl+D`）。选中几行就只处理那几行，每个操作一步就能撤销。
- 二进制文件和更大的文件只能查看。

**记住上次的样子**
- 关闭 AIEdit 时不再询问要不要保存：未保存的修改（包括没起名字的新文件）会自动留下，下次启动原样回来，撤销一步就回到磁盘上的版本。编辑时也会随时备份，程序崩溃了也能找回。关闭单个标签页时仍会询问。
- 文件菜单 →「最近打开」列出最近的 10 个文件。
- 每个文件都记住光标、读到哪里（屏幕顶端那一行）、视图模式和手动选过的编码，下次打开回到原处。读长篇小说也不用自己记页码。
- 不带文件启动时，恢复上次关闭时打开的那些标签页；从文件管理器打开某个文件时，只打开它。

**看日志**
- 文件变化时自动重新加载（有未保存的修改时不会覆盖它们）。
- 「跟随文件末尾」（`Ctrl+Shift+T`）的效果和 `tail -f` 一样。
- **在文件夹里查找**（`Ctrl+Shift+F`）：在一个文件夹（可含子文件夹）的所有文件里查找文本、正则，或者直接用过滤面板最近运行的规则，比如一次查完一个程序的全部日志。每个文件按它自己的编码读，二进制文件和隐藏文件夹会跳过；结果按文件分组，双击打开文件并跳到那一行。
- 日志被 logrotate 截断时不会崩溃。
- **过滤行**（`Ctrl+Shift+L`）：在文档下方只列出符合规则的行。规则由几条「包含」「排除」的正则表达式组成，还能比较捕获到的数字，比如 `took (?<ms>\d+)ms` 加上 `ms > 1000`，就只看耗时超过一秒的请求。
- **一句话让 AI 写规则**：比如「超过一秒的错误，不要心跳」。AI 只看到你的描述和最多 80 行样例（每行最多 200 个字符）；它写出的条件就摆在面板里，可以检查、修改，然后在本机对整个文件运行。
- 结果按日志级别着色（ERROR 红、WARN 橙、DEBUG 灰），匹配到的部分高亮。用方向键浏览时原文跟着跳到那一行；回车或双击回到原文继续看。`Ctrl+C` 复制选中的结果行。
- 过滤在后台进行，结果边找边显示；300 MB 的日志约 0.6 秒。日志变长时只读新增的部分，配合「跟随文件末尾」可以只盯着错误看。

**AI 助手**（`Ctrl+Shift+A`）
- 选中文本或字节，可以一键解释、翻译、总结；也可以直接提问和追问，回答以流式显示。
- 「这是什么文件」只把在本机计算的摘要（类型、熵、字符串、开头字节）发出去，不发送文件本身。
- 发送前可以预览完整的请求内容，面板会写明将附带什么。
- **AI 能自己查文件**：问「这个日志里为什么报错」时，AI 会调用本地工具在文件里搜索、读取需要的片段，不用把整个文件发出去；要统计时（「有多少次超时」），它会在本机跑过滤规则，只拿回行数和前几行。每一次调用都会显示在对话里。
- **字段标注**：AI 认出二进制结构后，会把字段用颜色标在十六进制视图上，鼠标悬停显示字段名和含义。
- 默认使用 DeepSeek，也支持其他 OpenAI 兼容服务（通义、Kimi、本地的 Ollama 等）。

设置 API key 有两种方式：
- 在菜单「AI → AI 设置」里填写。key 只保存在本机的 `~/.config/AIEdit/api-key`，只有你能读取。
- 用环境变量，此时 key 不会写入文件：

```bash
AIEDIT_API_KEY=你的key ./build/src/aiedit
```

**其他**：多标签页、拖放打开文件、缩放、中文界面。

| 操作 | 按键 |
|---|---|
| 文本 / 十六进制 / 并排 | `Ctrl+1` / `Ctrl+2` / `Ctrl+3` |
| 查找 / 下一个 / 上一个 | `Ctrl+F` / `F3` / `Shift+F3` |
| 替换 / 过滤行 | `Ctrl+H` / `Ctrl+Shift+L` |
| 上移、下移行 / 复制行 | `Alt+↑`、`Alt+↓` / `Ctrl+D` |
| 转到行号或偏移 | `Ctrl+G` |
| 重新加载 / 跟随文件末尾 | `F5` / `Ctrl+Shift+T` |
| 在文件夹里查找 | `Ctrl+Shift+F` |
| 十六进制视图里切换十六进制栏和字符栏 | `Tab` |
| 缩放 | `Ctrl++`、`Ctrl+-`、`Ctrl+0`，或 `Ctrl+滚轮` |
| AI 助手 / 解释选区 | `Ctrl+Shift+A` / `Ctrl+Shift+E` |

## 构建

依赖：
- 必需：CMake 3.21 或更高、支持 C++20 的编译器、Qt 6.5 或更高（Core、Gui、Widgets、Concurrent、Network、Test）。
- 可选：
  - KSyntaxHighlighting：语法高亮。
  - uchardet：更准确的编码识别。
  - Qt 的 `lrelease`：编译中文界面翻译。

**Ubuntu / Debian**

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-l10n-tools libkf6syntaxhighlighting-dev libuchardet-dev
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j
```

```bash
./build/src/aiedit 文件1 文件2
```

**Windows**：安装 Qt 6（MSVC 版本）和 CMake 后，用相同的 CMake 命令构建。Windows 版目前还没有验证过，已知的问题见 DESIGN.md 末尾。

## 安装到桌面（Linux）

安装到 `~/.local`，不需要 sudo：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DAIEDIT_BUILD_TESTS=OFF
```

```bash
cmake --build build-release -j && cmake --install build-release
```

然后注册到桌面：

```bash
packaging/linux/register.sh --default
```

注册后：
- 应用菜单里出现 AIEdit。
- 文件管理器右键 →「脚本」→「用 AIEdit 打开」，对任何文件都可用。
- 加上 `--default` 时，纯文本、日志、Markdown、JSON/XML/YAML、二进制和可执行文件默认用 AIEdit 打开。你已经指定给其他程序的类型（C/C++ 源码、shell 脚本、CSV）保持不变，因为 AIEdit 目前还不能编辑。

AIEdit 只保留一个窗口：再打开文件时，会在已有窗口里新开标签页。需要单独的窗口时用 `aiedit --new-window`。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

GUI 测试以 offscreen 方式运行，不会弹出窗口。如果设置了 `AIEDIT_SCREENSHOT_DIR`，测试会把截图保存到这个目录。

## 翻译

界面字符串在源码中用英文书写并包上 `tr()`。修改界面文字后，运行下面的命令更新 `translations/*.ts`，再补全译文（可以用 Qt Linguist）：

```bash
cmake --build build --target update_translations
```

## 许可证

[GPL-3.0-or-later](LICENSE)
