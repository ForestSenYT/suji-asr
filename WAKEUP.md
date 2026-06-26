# WAKEUP — 晨间摘要(2 分钟看完)· 2026-06-27

> 通宵自主推进结束。全部**本地提交,未 push**(main 领先 origin **34** 提交)。详见 `PROGRESS.md` 末「🌙 通宵自主推进」段 + `.superpowers/sdd/` 各报告。

## 1. 完成了什么(均已 build + 测试 + 看真实输出验证)
1. **GUI 空状态提示居中修复**(`b680619`)——你实测发现的那个左上角错位/截断,根因是视口尺寸没就绪就摆放;改成监听视口自身 resize 居中。
2. **⭐ 流式解码 = 多小时单文件的解法**(`71ce21a`)——以前整文件先全解码进内存(~230MB/小时)再转写;现在**边解码边 VAD 边推队列**,转写从第 ~2 秒就开始、内存只占一小段。逐字节不变性测试(163K 断言)+ 对抗评审 7 项全过。
3. **⭐ P5 长度分桶,门控到单文件**(`c40d2b0`)——单个长文件 **+16% 吞吐**(你的主场景),多文件**持平不回退**(N>1 走原 FIFO)。
4. **安装包 0.6 重建+验证**(`build\installer\suji-asr-setup-0.6.exe`,**3.7GB**,all-in-one)——含 **fp16+int8 两模型 + CUDA + Qt + ffmpeg + VC 运行时**;静默装干净目录、最小 PATH 跑通:自动 `provider=cuda` + fp16 GPU、转写正确。
5. **测试 188/188 绿,0 警告(/W4)**。

## 2. ⭐ 我抓到并修了 2 个"会让安装包在你 3070Ti 上直接坏掉"的 Critical(打包前对抗式 QA)
- **C1:VC++ 运行时没打包**——三个 exe + CUDA provider 硬依赖 MSVCP140/VCRUNTIME140,干净机(没装 VC++ Redist)会**起不来、CUDA 也加载失败**。dev 机能跑只因系统里已有——**之前 0.5 的"干净目录测试"也在 dev 机,所以一直没暴露**。已修:打包脚本拷 VC 运行时进包 + 缺失即中止(实证修好)。
- **C2:P5 的 N==1 门控其实从没进生产代码**——那个号称"门控"的提交**只加了个假测试**(自带队列、从不调真引擎),真引擎仍对所有文件无条件分桶 → 多文件 −3.5% 回归是活的。重做成真门控 + 真测试,实测多文件回到持平。

## 3. 我自主做的决定(可能你想确认 / 回退法)
- **P5 门控到「仅单文件(N==1)」启用**:因为你说主场景是单个多小时文件(那里 +16%),而多文件那点回归只在"很多短文件"才有。回退:删 `batch_engine.cpp` 里 `const bool bucket=(N==1)` 的门控。
- **安装包同时打包 int8+fp16 两个模型**(共 ~3GB → 包 3.7GB):适配机自适应(GPU 用 fp16、纯 CPU 用 int8)。若只在 3070Ti 用且不要 CPU 回退,可只留 fp16 砍到 ~2GB(改 `.iss`)。
- 流式解码"丢弃最后不足一窗的尾巴"与旧逻辑一致(保持输出逐字节相同)。

## 4. NEEDS-HUMAN(留给你 / 需硬件 / 需账号)
1. **`git push`**(挂机禁止)——main 领先 origin 34 提交。
2. **真 3070Ti(Ampere)上 benchmark**——确认 GPU 真比 CPU 快、fp16 路径在 Ampere 上跑(dev 是 2080/Turing,测不了 Ampere)。`scripts\benchmark.ps1`。
3. **在真·干净机(无 VS/无 CUDA/无 VC 运行时)装一次 0.6 跑通**——dev 机系统里有这些,掩盖问题;真干净机才是终极验证。
4. **GUI 真机观感/交互人工确认**(空状态、图标、各功能)。
5. **许可证合规**:打包的 VC 运行时、CUDA redist(~2.4GB)、ffmpeg-lgpl、FireRedASR/sherpa 模型,分发前核实。
6. 清理:测试安装目录 `F:\suji-install-test-06`(5.8GB,可删)。

## 5. 怎么自己验证(现在就能)
- **装好的版本**:`F:\suji-install-test-06\suji_gui.exe`(双击;或 `suji_batch.exe <视频/目录> -o <输出>`)——已实测从这装一份能自动用 GPU+fp16 转写。
- **源码版**:`F:\Git\suji-asr\build\Release\suji_gui.exe`。批量:`build\Release\suji_batch.exe "<目录>" -o build\out`。
- **测试**:`vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release` 然后 `build\Release\suji_tests.exe`(188/188)。
- **多小时文件**:现在流式解码,几秒就开始转、内存不爆——可拿一个长视频试。

## 6. 建议下一步
1. 决定 `git push`(34 提交)。
2. 把 `suji-asr-setup-0.6.exe` 拷到 3070Ti,装上跑一个真实长讲座,看吞吐 + 确认 GPU/fp16 生效。
3. 据 3070Ti 结果决定是否保留"两模型打包"或砍成 fp16-only。
4. 剩余 Minor(非阻塞,记录在 QA 报告):.iss 顺带打了几个用不到的 DLL(可加 Excludes 省体积)、GUI 分母在续跑时显示口径、`--selftest` 需带 wav 参数——都不影响正确性。
