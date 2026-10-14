# AIEdit 设计文档

## 定位

打开任何文件都秒开，自动选择合适的视图，看不懂就问 AI。以**查看为主、编辑为辅**，可以理解为带 AI 的现代版 Total Commander Lister / macOS Quick Look。

明确不做：LSP、调试、项目管理、插件市场。

## 已确定的决策

| 项目 | 决定 |
|---|---|
| 许可证 | GPL-3.0-or-later（可以复用 GPL 项目的代码，比如 klogg、Notepad++） |
| 技术栈 | C++20 + Qt 6 Widgets，CMake 构建 |
| 平台 | Linux（主力）和 Windows，代码保持 macOS 可移植 |
| AI | 先接 DeepSeek（OpenAI 兼容的 `/chat/completions` 接口，通义、Kimi、Ollama 等兼容服务也能用）；Claude 作为下一个 Provider |
| Markdown 视图 | 延后，先做 Hex、Text 和 AI |
| 界面语言 | 源码里的字符串用英文并包上 `tr()`，中文通过 `translations/*.ts` 提供 |
| 可选依赖 | KSyntaxHighlighting（语法高亮）和 uchardet（编码检测）。没装也能构建，只是少了对应功能 |

## 设计原则

1. **秒开最重要**：原生程序，文件用 mmap 打开，只绘制可见区域，开销与文件大小无关。
2. **一个文件，多种视图**：同一份字节在 Hex、文本、结构树之间切换和联动，默认界面保持干净。
3. **看懂二进制靠结构模板**：010 Editor 和 ImHex 的核心能力，后续用 `.ksy` 模板实现。
4. **编码识别要准**：GBK 和 UTF-8 乱码是中文用户的高频痛点，必须能一键换编码。
5. **AI 负责生成规则，本地负责执行**：AI 生成正则、过滤条件和结构模板，由本地引擎执行。大文件不用上传，结果可验证，也更省钱。
6. **一切以字节偏移为模型**：光标、选区和搜索结果都用文件里的字节偏移表示，各视图之间不需要换算层。

## 架构

```
src/
  core/   纯 C++20，不依赖 Qt，可单测，以后可复用到命令行工具
    FileSource   只读文件访问（mmap）
    FileType     文本/二进制、BOM、magic 识别
    TextCodec    编码的字节结构（字符边界）、默认编码选择（uchardet + 结构检查 + 地区偏好）
    LineIndex    行索引：每 256 行一个检查点，支持后台构建、边建边读
    Search       字节搜索（Boyer-Moore-Horspool，支持 ?? 通配，分块可取消）
    Offset       偏移和行号表达式解析
    Sse          SSE 流的增量解析（AI 流式输出）
    FileSummary  十六进制 dump、熵、字符串提取、文件摘要（「这是什么文件」）
  ui/     Qt Widgets
    HexView      十六进制视图
    TextDocument 某种编码下的文本：解码器 + 后台建索引
    TextDecoder  逐字符解码，记录每个 UTF-16 单元来自哪些字节
    TextView     文本视图（QTextLayout 排版、语法高亮）
    SearchJob    搜索任务（后台线程）
    FindBar      查找栏
    SyntaxHighlighting  KSyntaxHighlighting 的封装（库不存在时自动降级）
    RowScrollBar 64 位行号到 QScrollBar 的映射
    DocumentView 一个打开的文件：Text/Hex 视图、同步、查找栏、监视文件变化
    AiSettings   AI 服务设置；key 单独存放在仅本人可读的文件里，或从环境变量读取
    ChatClient   流式请求 /chat/completions，解析 SSE，把错误整理成可读的信息
    AiPanel      AI 助手面板：快捷操作、对话、附带内容和发送预览
    AiSettingsDialog
    MainWindow, DataInspector
tests/    Qt Test；GUI 测试以 offscreen 方式运行
translations/  界面翻译（.ts）
```

要点：
- **FileSource**：普通文件用 mmap（Windows 用 `MapViewOfFile`）；报告大小为 0 但有内容的文件（`/proc`、`/sys`）退回到读入内存。FIFO 和设备文件直接拒绝，用 `O_NONBLOCK` 打开，避免卡住。
- **LineIndex**：一行超过 4 KiB 时，在字符边界上拆成多个显示行，所以超长行（比如压缩过的 JSON）也能流畅显示。拆分规则是确定性的，从任意一行的开头往后扫，得到的结果和索引完全一致。
- **TextDecoder**：ICU 支持的旧编码（GB18030、Big5……）通过 `QStringDecoder` 解码，并缓存每个字符的解码结果；UTF-8/16/32 自己解码。控制字符显示为 ␀、␛ 这类可见符号，同时保持字节和字符的一一对应。
- **搜索**：精确文本先编码成文件的编码，再按字节搜索，落在字符中间的假匹配会被跳过；十六进制模式也走字节搜索。忽略大小写和正则表达式则按约 1 MiB 的行块解码，再用 `QRegularExpression` 匹配，所以所有编码都能用。
- **语法高亮**：4 MiB 以内的文件从文件开头开始精确计算高亮状态，每 64 行存一个检查点；更大的文件只往回看 256 行，结果是近似的，但开销与文件大小无关。
- **重新加载**：监视文件变化（`QFileSystemWatcher`）。文件增长时等写入告一段落（250 ms）再加载；文件变小时立即重新映射，缩短旧映射越过文件末尾的时间窗口。跟随模式下，每次加载后跳到文件末尾。

## 核心视图

- **Hex**（M1）：偏移、十六进制、字符三栏；按字节类别着色；键盘和鼠标选择；复制为十六进制、文本、C 数组或偏移；数据检查器；每行 8、16 或 32 字节。
- **Text**（M2）：大文件后台索引；行号；中日韩文字、emoji 和 Tab 正确排版；编码切换（20 种）；语法高亮（300 多种语言）；双击选词；转到行号。
- **联动**：文本、十六进制、并排三种模式（Ctrl+1/2/3），光标和选区始终同步。
- **查找**（M2）：Ctrl+F，可选文本、正则、十六进制三种模式；F3 / Shift+F3 查找下一个、上一个，到头自动绕回。
- **默认只读**，需要修改时再进入编辑（M4）。

## AI 功能（M3a 已完成，M3b 进行中）

按价值排序：

1. **识别文件类型**：本地先算出文件头、magic、熵分布和字符串样本，只把这份摘要发给 AI。
2. **推断二进制结构**：AI 写 `.ksy` 模板，交给本地解析器验证，出错就把错误信息发回去让它改，成功后把结果叠加显示在 Hex 视图上。
3. **用自然语言过滤日志**：AI 生成过滤表达式，在本地对大文件执行。
4. **选中就能问**：对选中的文字或字节做解释、翻译、总结。
5. **处理长文档**：总结、翻译、问答。
6. **修复乱码**：本地列出常见的错误编码组合，由 AI 判断哪个结果通顺。

已实现（M3a）：右侧的「AI 助手」面板。
- **快捷操作**：解释、翻译、总结、这是什么文件。
- **流式回答**：用 Markdown 渲染，支持表格和代码块。
- **追问**：带上最近 8 轮对话。
- **附带内容**：选区最多附带 32 KiB 文本，或 4 KiB 字节的十六进制 dump；面板会写明附带了什么，超出部分会注明只附带开头。
- **「这是什么文件」**：只发送本机算出的摘要（类型、熵、字符串、开头 256 字节），不发送文件本身。
- **发送预览**：显示将要发送的完整 JSON。API key 只放在请求头里，不会出现在预览中。
- **API key 的保存**：key 保存在 `~/.config/AIEdit/api-key`，文件一创建就是 0600 权限；也可以改用环境变量 `AIEDIT_API_KEY` 或 `DEEPSEEK_API_KEY`，此时不会写入文件。

接入方式：

- 现在只有一个 OpenAI 兼容的客户端（`ChatClient`）：用 Qt Network 发 HTTP 请求，再用 core 的 `SseParser` 解析流。DeepSeek 默认开启思考模式，所以快捷操作会显式发送 `thinking: disabled` 来加快速度；设置里可以打开「深度思考」。
- 下一步抽象出 `LlmProvider` 接口，再接 Claude 的 Messages API。Anthropic 没有官方 C++ SDK，同样直接走 HTTP + SSE。
- 默认模型 `deepseek-flash`（便宜、快），可选 `deepseek-v4-pro`。
- 小文件直接放进上下文。大文件既不截断也不整篇上传，而是提供本地工具让模型按需读取：`file_info()`、`read_bytes(offset, len)`、`read_lines(from, n)`、`search(pattern)`。这些工具可以直接复用现有的 core（LineIndex、Search）。
- 用结构化输出返回字段标注 `{offset, length, name, type, note}`。
- Prompt caching：系统提示、工具定义和文件摘要放在请求前面且保持不变。
- 隐私：发送前可预览发送内容；可设置某些路径永不发送；自动遮住 API key 一类的字符串；API key 存在系统钥匙串里。

## 技术选型

| 模块 | 选择 | 状态 |
|---|---|---|
| 框架 | C++20 + Qt 6 Widgets | ✅ |
| 文件读取 | mmap（以后加 piece table 支持编辑） | ✅ mmap |
| Hex 视图 | 自绘 `QAbstractScrollArea` | ✅ |
| 文本视图 | 自绘（`QAbstractScrollArea` + `QTextLayout`） | ✅ |
| 语法高亮 | KSyntaxHighlighting（MIT，可选） | ✅ |
| 编码识别 | uchardet（可选）+ 自己的字节结构检查 + 地区偏好；解码用 Qt/ICU | ✅ |
| 类型识别 | 自带的 magic 表；以后可接 libmagic | ✅ 基础版 |
| 搜索 | 字节搜索（`std::boyer_moore_horspool_searcher`）+ `QRegularExpression`（Qt 内置 PCRE2） | ✅ |
| 结构模板 | 自己写 `.ksy` 解释器 + yaml-cpp | M4 |
| AI 接入 | Qt Network 发 REST 请求 + SSE | M3 |
| Markdown | md4c + litehtml | 延后 |

## 路线图

| 阶段 | 内容 | 状态 |
|---|---|---|
| M1 | 项目框架、mmap、类型识别、HexView、数据检查器、多标签页、中文界面 | ✅ 完成 |
| M2 | 文本视图（大文件、长行）、语法高亮、编码识别和切换、搜索（文本、正则、hex）、Text 与 Hex 联动、自动重新加载和跟随模式 | ✅ 完成 |
| M3a | AI 面板：DeepSeek / OpenAI 兼容接口、流式输出、解释/翻译/总结选区、这是什么文件、发送预览、key 安全保存 | ✅ 完成 |
| M3b | 让 AI 用本地工具按需读取大文件（`read_bytes`、`search`……）、用结构化输出标注字段并显示在 Hex 视图上、Claude Provider | 下一步 |
| M4 | 编辑和安全保存（piece table）、`.ksy` 模板 + AI 生成模板、文件对比 | |
| 以后 | Markdown 视图、JSON/CSV 视图、本地模型、插件、字符栏按编码显示 | |

## 已知限制和风险

- **文件被截断时可能 SIGBUS**：POSIX 上如果其他进程截断了已映射的文件，读取超出新末尾的页会触发 SIGBUS。检测到文件变小会立即重新映射，缩短了时间窗口，但后台建索引的线程仍可能在这期间读到被截掉的部分。彻底解决需要给读取加上 SIGBUS 保护（`sigsetjmp` + 信号处理）。
- **Windows 还没编译验证过**：`FileSource` 的 Win32 实现已经写好，但没有在 Windows 上构建过。另外，Qt 官方的 Windows 版本通常不带 ICU，GB18030 等旧编码需要改用 Win32 的 `MultiByteToWideChar` 来解码。
- **正则匹配不能跨越约 1 MiB 的行块**；拆分过的超长行也会切断匹配。
- **4 MiB 以上文件的语法高亮是近似的**：只往回看 256 行，所以很长的块注释内部可能着色不对。
- **Hex 视图的字符栏只显示 ASCII**：多字节字符显示为 `.`。需要多字节字符时，可以切到文本视图或并排模式。
- **编码识别是启发式的**：短文本里 GBK 和 Big5 本来就难以区分，所以保留了一键换编码。
