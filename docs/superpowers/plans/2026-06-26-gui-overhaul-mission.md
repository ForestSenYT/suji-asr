# MISSION PROMPT — suji-asr GUI 大改:设计感 + 交互 + 细致进度 + 每文件独立 + 详尽日志(2026-06-26)

> 交给自主编码 agent 端到端执行。本文档即全部上下文。GUI **视觉/交互正确性属 NEEDS-HUMAN**(无头测不了观感)——但每步都要 build + 全测试通过 + headless `--selftest` 不崩 + 尽量 offscreen 截图。**不破坏引擎/并发正确性。不 push。** 美学部分**先调用 `frontend-design` skill** 拿设计方向。

## 1. 现状
suji-asr 的 Qt6 Widgets GUI(`src/gui/main_window.{h,cpp}`、`engine_worker.{h,cpp}`)目前:顶部工具栏(添加文件/文件夹/清空/输出目录)、中间一个 `QTableView`+`QStandardItemModel`(列:文件/状态/段数/错误)、下方一个 `QPlainTextEdit` 日志面板、设置行(推理后端 combo、SRT/VTT/JSON/MD 勾选、批大小/并行文件 spinbox)、一个**全局** `QProgressBar`+状态标签、开始/取消按钮。引擎实时回调 `BatchProgress{files_total,files_done,segs_done,segs_total,audio_seconds_done,total_audio_decoded}`(`src/core/batch_engine.h`),`core/log.h` 的 `log_info/err` 经 sink 流到日志面板。引擎是异构 work-stealing(CPU+GPU 双消费者),**每个 segment 带 file_id**,已有 per-file 的 `seg_pending[fid]`(R6)与全局 `segs_done/segs_total`。

**已具备但要升级的点**:进度是**全局**一条(`segs_done/segs_total`),所有文件共用;日志是粗粒度(provider、总时长、每文件完成);外观是 Qt 默认控件样式。

## 2. 目标(用户原话)
让前端**更有设计感、交互体验更棒、日志更详细频繁、进度条更细致、每个视频分开**。

## 3. 任务(按依赖顺序;A 是结构基础)

### A —— 【引擎+GUI,结构性】每个视频独立进度("每个视频分开")
当前进度是一条全局条。改成**每个文件一行、各有自己的进度**。
1. **引擎上报 per-file 进度**(`src/core/batch_engine.cpp` 两条路径 single+hetero,**不破坏并发正确性**):维护**每文件**计数 `segs_total_per_file[fid]`(生产者 push 该文件每个段时 +1)和 `segs_done_per_file[fid]`(消费者路由该段 token 后 +1,按 file_id)。在已有的节流回调里附带一份快照:给 `BatchProgress` 增 `std::vector<FilePstat> files;`,`struct FilePstat { int file_index; long long segs_done; long long segs_total; };`(只含已开始/已知的文件即可)。**这些计数都是 atomic / 在已有 err_mu 下,别引入新 race**;sum 校验:Σsegs_done_per_file == 全局 segs_done。
2. **派生每文件阶段(phase)**(不必显式追踪,按计数派生):`segs_total==0` → 待处理/解码中;`0<done<total` → 转写中;`done==total 且 finalize 完` → 完成;失败/取消按现有 `FileResult` 区分。可选:生产者在 decode/VAD 开始时 `log_info` 一行(见 C),GUI 据此把行标到"解码中/切分中"。
3. **GUI 每行进度条**:给表格加一列「进度」,用 `QStyledItemDelegate` 在该列**绘制一个进度条**(显示该文件 `segs_done/segs_total` 的 %、或忙碌动画当 total 未知)。`onWorkerProgress` 收到 `files` 快照后更新对应行的进度/百分比/段数/速度/ETA(每文件各算各的)。状态列同步("待处理→处理中→完成/失败/取消")。
4. 底部保留一条**总进度**(所有文件聚合 %)+ 全局 ETA;表格内是**每文件**进度。
- 测试:引擎 per-file 计数正确(Σ 校验、双消费者无重复);GUI delegate 渲染逻辑可单测的部分。并发测试(double-processing/cancel/R3/R6/hetero smoke)全绿。

### B —— 【GUI】进度更细致
- 每文件进度:除 %,显示**实时速度(倍速)+ 该文件 ETA**(用已合并的"从转写阶段算"的口径,每文件独立锚点)。
- 平滑:每秒定时器在两次引擎回调之间**插值推进**每行进度条,避免大 batch 时冻住(别越过真实值)。
- 阶段细分(可选):一行内用文字/颜色标「解码中/切分 N 段/转写 X%/标点/写盘」。
- 总进度条用确定式百分比(聚合 segs),能走到 100%。

### C —— 【引擎+GUI】日志更详细、更频繁
- **引擎多发 log_info**(带文件名,UTF-8):每文件 `解码 <name>` → `切分完成 <name>(N 段)` → 转写里程碑(如每 25%)→ `完成 <name>(CPU x%/GPU y%,Z 倍速,输出 → <路径>)`;失败/取消/回退(源目录不可写改存、fp16 选用)已有的也保留。频率适中(节流,别刷屏)。
- **GUI 日志面板升级**:① 每行带**时间戳**(`[HH:MM:SS]`);② **按级别上色**(INFO 灰、OK 绿、ERR 红/加粗、阶段 青);③ 自动滚动到底 + 一个「清空日志」「复制」按钮;④ 等宽字体;⑤ 可选级别过滤(只看错误)。用 `QTextEdit`(富文本上色)或给 `QPlainTextEdit` 配 `QSyntaxHighlighter`。日志 sink 把 level 透传给上色逻辑。

### D —— 【GUI,NEEDS-HUMAN 验收】设计感(先用 frontend-design skill)
**先 `Skill: frontend-design`** 拿方向,再落地一套**统一 QSS 主题**(深色,匹配用户偏好),要点:
- 配色:沉稳深色背景 + 一个明确强调色(进度/按钮/选中),足够对比;避免默认灰控件的"模板感"。
- 排版:字号/字重层级(标题/正文/次要信息)、行距、留白(统一 margin/padding)。
- 控件:圆角按钮 + hover/pressed 态、细致的进度条样式、干净的表格(行高、交替色、表头)、设置行对齐美观、日志面板边框/背景。
- 一个**应用图标** + 窗口标题区;**空状态**(无文件时表格中央提示"拖入视频 或 点添加文件")。
- 整体一致、克制、有意图(frontend-design 的原则),不是堆效果。
> 这部分**外观对错只能人工确认**(NEEDS-HUMAN)。agent 负责实现 + 保证编译/不崩 + offscreen 截图留证;最终观感交用户定。

### E —— 【GUI】交互体验
- 拖拽:拖文件到窗口时**高亮**放置区(`dragEnterEvent` 视觉反馈)。
- 状态明确:无文件时「开始」禁用;运行中「开始」禁用、「取消」启用;空闲反之。
- 工具提示 + 快捷键(如 Ctrl+O 添加文件、Esc 取消)。
- 行右键菜单:移除该行、重试该文件、**打开输出文件夹/打开转写结果**;双击完成的行→打开其输出。
- 可选:「完成后自动打开输出文件夹」勾选;完成时状态栏给个明确收尾(成功/失败计数 + 总耗时 + 输出位置)。
- 这些都要保持取消/重入安全(worker 在 QThread,信号 queued;沿用现有直接 `requestCancel()` 模式)。

## 4. 执行纪律
- 分支 + 本地提交;**不 push**。TDD:引擎 per-file 计数、GUI 可测逻辑(delegate 数据、ETA 计算、日志上色映射)先写测试;视觉留 NEEDS-HUMAN。每步 build + 全测试绿 + `suji_gui --selftest`/`--selftest-gui` 不崩(信号签名若变,更新所有 selftest 调用点)。**不破坏引擎/异构/取消/fp16-auto/输出回退**。`/W4 /utf-8` 干净。维护 PROGRESS.md。
- 信号演进:`progress` 若要带 per-file 快照,改 `EngineWorker::progress` 签名并更新 `main_window` 槽 + `main.cpp` 所有 selftest lambda 调用点(`Q_ARG` 对齐)。
- 美学先 `frontend-design` skill;subagent-driven(implementer+reviewer)。

## 5. 验收 / 交付
- 每文件独立进度(表内每行一条进度条 + %/速度/ETA/段数)实测在 3-5 个真实文件上正确分开显示。
- 日志:带时间戳 + 上色 + 自动滚动 + 每文件阶段事件,频率合适。
- 进度条细致、平滑、能到 100%。
- 一套统一深色主题 + 空状态 + 图标 + 拖拽高亮 + 右键打开输出。
- 全测试绿;headless selftest 通过;offscreen 截图若干。
- **NEEDS-HUMAN**:在真机点开看观感/交互(拖拽、右键、每行进度、日志上色、整体美感),用户拍板;visual 调整按反馈迭代。

> 开始:先 A(引擎 per-file 进度 + 表内每行进度条)——这是"每个视频分开"的地基;再 B/C(细致进度 + 详尽日志);最后 D/E(美学 + 交互,先 frontend-design)。每步 build+测试+不崩,视觉交用户确认。
