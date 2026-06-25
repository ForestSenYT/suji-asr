# Phase 5 — Qt 6 Widgets 桌面 GUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]`.
> ⚠️ **GUI 视觉正确性需人工确认**(挂机只能验"编译 + 启动不崩 + 信号连通");最终外观留 NEEDS-HUMAN。

**Goal:** 给非开发者同事用的批处理仪表盘:拖拽文件/夹 → 队列表(每文件状态/进度/错误)→ Start/Cancel → 全局进度+聚合吞吐+ETA → 打开输出目录。驱动 `suji_core` 的 `transcribe_batch_files`,**引擎在 worker 线程跑,不卡 UI**,**支持取消**。

**Architecture:** `suji_gui`(Qt6 Widgets exe)链接 `suji_core`。`MainWindow` 持有一个 `EngineWorker`(QObject 移到 QThread);worker 调 `transcribe_batch_files`,通过 Qt 信号(队列连接)把 `BatchProgress` / 每文件结果发回 UI 线程更新控件。取消用引擎新加的 `std::atomic<bool>* cancel` 标志。

**Tech Stack:** C++17 · Qt 6.8 Widgets(`F:\Qt\6.8.1\msvc2022_64`)· CMake `find_package(Qt6)` · 复用 `suji_core`。

## Global Constraints
- 平台 Windows x64,MSVC,C++17,`/utf-8`。cmake 全路径同前。Qt 前缀:`-DCMAKE_PREFIX_PATH=F:/Qt/6.8.1/msvc2022_64`(或 `Qt6_DIR`)。
- 复用 `suji_core`:`probe_hardware`/`decide`、`transcribe_batch_files`/`BatchProgress`/`FileResult`、`EngineConfig`、`write_outputs`、`transcript_complete`(Phase 3,若已合)。
- 引擎逻辑不在 GUI 重写;GUI 只是壳 + 线程编排。
- `/W4` 对我方代码 pristine(Qt 头文件警告可隔离)。

## File Structure
```
src/core/cancel.h                 # struct CancelToken { std::atomic<bool> cancelled{false}; }
src/core/batch_engine.h .cpp      # (MODIFY) transcribe_batch_files 增可选 CancelToken* 参数
src/gui/main.cpp                  # QApplication + MainWindow
src/gui/main_window.h .cpp        # 仪表盘:输入列表、队列表、进度、设置、Start/Cancel
src/gui/engine_worker.h .cpp      # QObject worker:跑引擎,发 progress/fileDone/finished 信号
tests/test_cancel.cpp            # 引擎取消的集成测试(无 GUI)
```
CMake:`option(SUJI_BUILD_GUI ...)`;若找到 Qt6 则加 `suji_gui`(`qt_standard_project_setup`/AUTOMOC),`target_link_libraries(suji_gui PRIVATE suji_core Qt6::Widgets)`,POST_BUILD 拷 sherpa DLL + (打包时)windeployqt。

---

## Task 1: 引擎取消支持(可测,无 GUI)

**Files:** Create `src/core/cancel.h`; Modify `src/core/batch_engine.{h,cpp}`; Create `tests/test_cancel.cpp`

**Interfaces:**
```cpp
// cancel.h
#pragma once
#include <atomic>
namespace suji { struct CancelToken { std::atomic<bool> cancelled{false}; void cancel(){cancelled.store(true);} bool is_cancelled() const {return cancelled.load();} }; }
// batch_engine.h: add trailing optional param
std::vector<FileResult> transcribe_batch_files(const std::vector<std::string>& inputs,
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb = nullptr, CancelToken* cancel = nullptr);
```
- 实现:producer 取文件循环里、consumer 取 batch 循环里,每轮检查 `cancel && cancel->is_cancelled()` → 提前结束(producer 停止取新文件;consumer 停止取新 batch 并让 pop 走 close)。已取消时:已完成文件正常收尾,未处理文件 `ok=false, err="cancelled"`。**取消必须不死锁**(取消后仍要 close 队列让消费者退出)。
- 测试 `test_cancel.cpp`(CPU):起一个线程跑 `transcribe_batch_files`(多个 wav)并传 CancelToken;另一线程很快 `cancel()`;join;断言函数返回(没卡死)、且至少有文件被标 cancelled 或 ok(不崩、不泄漏)。用 `doctest::timeout(120)`。
- 验收:`-tc="cancel*"` 绿;全套绿。commit `feat: engine cancellation via CancelToken`。

---

## Task 2: Qt 项目接入 + 空窗口

**Files:** Modify `CMakeLists.txt`; Create `src/gui/main.cpp`, `src/gui/main_window.{h,cpp}`

- CMake:`find_package(Qt6 COMPONENTS Widgets QUIET)`;`if(Qt6_FOUND)` 才加 `suji_gui`(否则跳过,不破坏现有构建)。`set(CMAKE_AUTOMOC ON)` 局部。
- `main.cpp`:`QApplication a(argc,argv); MainWindow w; w.show(); return a.exec();`
- `MainWindow`:空 `QMainWindow`,标题 "suji 批量转写",一个占位 central widget。
- 验收:配置时带 `-DCMAKE_PREFIX_PATH=F:/Qt/6.8.1/msvc2022_64` 能构建出 `suji_gui.exe`;**启动后窗口出现、不崩**(挂机:`Start-Process suji_gui.exe; sleep 2; 检查进程在 → Stop-Process`)。commit `build: Qt6 suji_gui target + empty MainWindow`。

---

## Task 3: 仪表盘控件(静态布局)

**Files:** Modify `src/gui/main_window.{h,cpp}`

- 布局:顶部工具条(Add Files / Add Folder / Clear / Output Dir…);中部 `QTableView`(列:文件名、状态、段数、错误)由 `QStandardItemModel` 驱动;底部:全局 `QProgressBar` + 标签(已完成/总数、吞吐 ×realtime、ETA)+ 设置(provider 下拉 auto/cpu/cuda、输出格式勾选)+ Start / Cancel 按钮。
- 拖拽:`setAcceptDrops(true)` + `dragEnterEvent`/`dropEvent` 收文件/夹加入列表。
- Add Folder 用 `QFileDialog::getExistingDirectory`,枚举媒体扩展名加入。
- 此 Task 只搭 UI + 列表管理(Start 先连一个 stub)。验收:构建 + 启动 + 能加文件进表(人工看)。commit `feat(gui): dashboard layout + input list + drag-drop`。

---

## Task 4: worker 线程接引擎 + 进度/取消

**Files:** Create `src/gui/engine_worker.{h,cpp}`; Modify `src/gui/main_window.{h,cpp}`

- `EngineWorker : QObject`:`public slot run(QStringList inputs, EngineConfig cfg)`;内部 `probe_hardware`+`decide`(或用 UI 设置),持有 `CancelToken cancel_`,调 `transcribe_batch_files(..., cb, &cancel_)`;cb 里 `emit progress(BatchProgress)`(注意:cb 在引擎线程,用 `Qt::QueuedConnection` 发到 UI);结束 `emit finished(QVector<FileResult>)`。`public slot cancel()` → `cancel_.cancel()`。
- 把 worker `moveToThread(new QThread)`;Start → `emit startRequested(...)`(连到 worker.run);Cancel → worker.cancel()。
- UI:progress 信号更新进度条/吞吐/ETA;finished 信号更新表格每行状态 + 写出(`write_outputs`)+ 弹"完成,打开输出目录"。注册 `qRegisterMetaType` 给跨线程的自定义类型(BatchProgress/FileResult/EngineConfig)。
- 验收:挂机——构建 + 启动 + 程序性触发一次小批(若可脚本化)或人工跑 test_wavs 一遍,确认进度动、不卡 UI、能取消、产出文件。**视觉/交互正确性 NEEDS-HUMAN。** commit `feat(gui): engine on worker thread + progress + cancel`。

---

## Phase 5 完成验收
- `suji_gui.exe` 构建、启动、加文件、Start 跑出输出、进度动、Cancel 生效、不卡 UI。
- 引擎取消有集成测试(无 GUI)绿。
- ⚠️ 外观/交互细节由人工在早晨确认(挂机仅保证编译 + 启动 + 信号连通 + 不崩)。

## Self-Review / 风险
- GUI 视觉无法挂机验证 → 明确标 NEEDS-HUMAN。
- 跨线程信号需 `qRegisterMetaType` + 队列连接(易错点)。
- 取消的死锁风险:取消后必须 close 队列让消费者退出(Task 1 重点测)。
- 若 Qt 未装/找不到:`suji_gui` 跳过,不影响 CLI/引擎构建。
