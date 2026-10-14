# cppEdit 设计文档

## 定位

打开任何文件都秒开，自动选择合适的视图，看不懂就问 AI。以**查看为主、编辑为辅**，可以理解为带 AI 的现代版 Total Commander Lister / macOS Quick Look。

明确不做：LSP、调试、项目管理、插件市场。

## 已确定的决策

| 项目 | 决定 |
|---|---|
| 许可证 | GPL-3.0-or-later（可以复用 GPL 项目的代码，比如 klogg、Notepad++） |
| 技术栈 | C++20 + Qt 6 Widgets，CMake 构建 |
| 平台 | Linux（主力）和 Windows，代码保持 macOS 可移植 |
| AI | 先接 Claude，Provider 抽象层留出 OpenAI 兼容接口和本地模型的位置 |
| Markdown 视图 | 延后，先做 Hex、Text 和 AI |
| 界面语言 | 源码里的字符串用英文并包上 `tr()`，中文通过 `translations/*.ts` 提供 |

## 设计原则

1. **秒开最重要**：原生程序，文件用 mmap 打开，只绘制可见区域，开销与文件大小无关。
2. **一个文件，多种视图**：同一份字节在 Hex、文本、结构树之间切换和联动，默认界面保持干净。
3. **看懂二进制靠结构模板**：010 Editor 和 ImHex 的核心能力，后续用 `.ksy` 模板实现。
4. **编码识别要准**：GBK 和 UTF-8 乱码是中文用户的高频痛点，必须能一键换编码。
5. **AI 负责生成规则，本地负责执行**：AI 生成正则、过滤条件和结构模板，由本地引擎执行。大文件不用上传，结果可验证，也更省钱。

## 架构

```
src/
  core/   纯 C++20，不依赖 Qt：文件读取、类型识别、偏移解析……可单测，以后可复用到命令行工具
  ui/     Qt Widgets：HexView、DataInspector、DocumentView、MainWindow
  main.cpp
tests/    Qt Test；GUI 测试以 offscreen 方式运行
translations/  界面翻译（.ts）
```

- `core::FileSource`：只读文件访问。普通文件用 mmap（Windows 用 `MapViewOfFile`）；报告大小为 0 但有内容的文件（`/proc`、`/sys`）或不支持 mmap 的文件系统，退回到读入内存（上限 256 MiB）。FIFO 和设备文件直接拒绝，用 `O_NONBLOCK` 打开，避免卡住。
- `core::detectFileType`：先查 BOM，再查 magic（约 50 种格式），再按内容判断是文本还是二进制：看是否有 NUL、是否是合法 UTF-8、控制字符比例，以及不带 BOM 的 UTF-16。只读取文件开头 64 KiB。
- `ui::HexView`：自绘的 `QAbstractScrollArea`。行号用 64 位；行数超过 int 范围时滚动条按比例缩放（已用 1 TiB 稀疏文件测试过）。光标始终落在某个字节上，选区是锚点到光标的闭区间。
- `ui::DocumentView`：一个打开的文件，持有 `FileSource`、类型信息和各个视图（M2 加入文本视图）。

## 核心视图

- **Hex**（M1 已完成）：偏移、十六进制、字符三栏；按字节类别着色；当前行高亮；键盘和鼠标选择，拖出视图时自动滚动；复制为十六进制、文本、C 数组或偏移；跳转到偏移（支持十六进制、十进制和相对偏移）；数据检查器；每行 8、16 或 32 字节；缩放。
- **Text**（M2）：自绘文本视图，后台建行索引，只排版可见区域，超长行分段排版；语法高亮；编码识别和切换；正则搜索；像 `tail -f` 那样跟随文件尾部。
- **联动**：Text 和 Hex 并排时光标同步。
- **默认只读**，需要修改时再进入编辑。

## AI 功能（M3）

按价值排序：

1. **识别文件类型**：本地先算出文件头、magic、熵分布和字符串样本，只把这份摘要发给 AI。
2. **推断二进制结构**：AI 写 `.ksy` 模板，交给本地解析器验证，出错就把错误信息发回去让它改，成功后把结果叠加显示在 Hex 视图上。
3. **用自然语言过滤日志**：AI 生成过滤表达式，在本地对大文件执行。
4. **选中就能问**：对选中的文字或字节做解释、翻译、总结。
5. **处理长文档**：总结、翻译、问答。
6. **修复乱码**：本地列出常见的错误编码组合，由 AI 判断哪个结果通顺。

接入方式：

- 抽象 `LlmProvider` 接口（流式对话 + 工具调用）。第一个实现接 Claude 的 Messages API。Anthropic 没有官方 C++ SDK，所以直接用 Qt Network 发 HTTP 请求并解析 SSE。
- 默认模型 Claude Opus 5.5（`claude-opus-5-5`），按任务设置 `output_config.effort`：解释、翻译这类快速任务用 `low`，推断结构用 `high`。
- 小文件直接放进上下文。大文件既不截断也不整篇上传，而是提供本地工具让模型按需读取：`file_info()`、`read_bytes(offset, len)`、`read_lines(from, n)`、`search(pattern)`。
- 用结构化输出返回字段标注 `{offset, length, name, type, note}`。
- Prompt caching：系统提示、工具定义和文件摘要放在请求前面且保持不变。
- 隐私：发送前可预览发送内容；可设置某些路径永不发送；自动遮住 API key 一类的字符串；API key 存在系统钥匙串里。

## 技术选型

| 模块 | 选择 | 状态 |
|---|---|---|
| 框架 | C++20 + Qt 6 Widgets | ✅ |
| 文件读取 | mmap（以后加 piece table 支持编辑） | ✅ mmap |
| Hex 视图 | 自绘 `QAbstractScrollArea` | ✅ |
| 文本视图 | 自绘（`QAbstractScrollArea` + `QTextLayout`） | M2 |
| 语法高亮 | KSyntaxHighlighting（MIT） | M2 |
| 编码识别 | uchardet；转换用 ICU/iconv | M2 |
| 类型识别 | 自带的 magic 表；以后可接 libmagic | ✅ 基础版 |
| 正则搜索 | PCRE2（开启 JIT），直接在 mmap 映射的字节上搜 | M2 |
| 结构模板 | 自己写 `.ksy` 解释器 + yaml-cpp | M4 |
| AI 接入 | Qt Network 发 REST 请求 + SSE | M3 |
| Markdown | md4c + litehtml | 延后 |

## 路线图

| 阶段 | 内容 | 状态 |
|---|---|---|
| M1 | 项目骨架、mmap、类型识别、HexView、数据检查器、多标签页、中文界面 | ✅ 完成 |
| M2 | 文本视图（大文件、长行）、语法高亮、编码识别和切换、搜索（文本、hex、正则）、Text 与 Hex 联动 | 下一步 |
| M3 | AI 面板：Provider 抽象 + Claude、流式输出、选中就问、本地工具、发送预览 | |
| M4 | 编辑和安全保存（piece table）、`.ksy` 模板 + AI 生成模板、文件对比 | |
| 以后 | Markdown 视图、JSON/CSV 视图、本地模型、插件 | |

## 已知限制和风险

- **文件被截断时可能 SIGBUS**：在 POSIX 系统上，如果另一个进程截断了已映射的文件，访问超出新文件末尾的页会触发 SIGBUS。计划：监视文件变化并重新映射，必要时为读取加 SIGBUS 保护。
- **Windows 代码路径还没编译验证过**：`FileSource` 的 Win32 实现已经写好，但还没有在 Windows 上构建。
- **字符栏目前只显示 ASCII**：多字节字符显示为 `.`。M2 让字符栏跟随编码设置。
- **编码识别是启发式的**：不可能百分之百准确，所以必须提供一键换编码。
- **自绘文本视图（M2）是最大的技术风险**：大文件、超长行、中文 / emoji / 组合字符、输入法要同时做好。
