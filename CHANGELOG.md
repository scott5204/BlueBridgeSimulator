# 更新日志（CHANGELOG）

**项目**：BlueBridgeSimulator — CT117E-M4（蓝桥杯嵌入式竞赛板，STM32G431RBT6）全软件模拟器
**架构**：QEMU/Unicorn 保真 CPU（真实 ARM Cortex-M4 指令执行）+ 自研 STM32G431 外设模型 +
自研板级模型（LCD/按键/LED/信号源/串口终端）+ Qt 6 GUI，四层解耦（CPU / STM32 / Board / GUI）。

版本按开发阶段记录，最新在上。

---

## 阶段七-2B.2 — Keil AGDI ↔ Debug IPC（Registers / Memory / Reset / Step，2026-09-27）

### 新增（`BlueBridgeSimulator/tools/keil_agdi/`，不参与主 CMake / ctest）

- `BlueBridgeAGDI/src/IpcClient`（+ `IpcProtocol`）：**唯一 reader 线程** +
  requestId→pending 映射 + write mutex + **绝对截止时间**超时（读写各自独立
  OVERLAPPED 事件，断线检测不误判）、`TARGET_STOPPED` 事件队列；
  日志经 `[IPC] …` 前缀进驱动日志（`--debug-ipc-trace` 可加详）。
- `AgdiSession`：连接/断开；HELLO + GET_CAPABILITIES 校验（Cortex-M4F、小端、
  flash `0x08000000`/128 KiB、必需能力位）；`AutoStart=1` 时
  `CreateProcessW bluesim.exe --debug-pipe <每会话唯一管道> --wait-debugger`，
  `PipeName=` 可选固定管道（自动化专用）；`LeaveSimulatorRunning=1` 保留模拟器；
  断线后驱动"装死"（AG_NOACCESS），**Keil 不崩**。
- `AgdiRegisters`：µVision 寄存器视图（`AG_CB_INITREGV`，4 组 / 64 项，含 FPU
  S0–S31+FPSCR，xPSR 位项按位读写）、`AG_AllReg`（官方 RgARM 块）、
  `AG_RegAcc`（0x00–0x0F / mPC / 0x10）；刷新 = **一条批量 READ_REGISTERS**，
  PC 经 `pCURPC` 回写。
- `AgdiMemory`：`AG_MemAcc`（READ/WRITE/RDOPC/WROPC → 7-2A 内存命令；flash 写
  一律拒绝 = `AG_RO`；`F_*` 留 B.4）、`AG_MemAtt`
  （`AG_MEMMAP` / `AG_GETMEMATT` / `AG_SETMEMATT`，见"根因"）。
- `AgdiDriver`：Reset = `AG_Init(AG_EXECITEM|AG_RESET)` → `RESET_HALT` + 寄存器
  刷新 + `U $` 反汇编；Step = `AG_GoStep(AG_NSTEP)` → IPC STEP（官方同步语义，
  不自行做 PC 算术）；Run/断点保持 B.1 答复（B.3）。
- `BlueBridgeAGDIProbe.exe`（`BlueBridgeAGDIProbe/src/probe_main.cpp`）：MSVC x86
  独立探针，复用同一 IpcClient/IpcProtocol、不经 Keil 即可验证
  "MSVC x86 ↔ 命名管道 ↔ MinGW64 bluesim"；12 项测试 × `--rounds`。
- `run_keil_session_stress.ps1` 扩展 `-PipeName` / `-SimulatorExe` 固定管道模式：
  预启动单个模拟器、备份并改写 `agdi.ini`（AutoStart=0 + PipeName）、
  结束后停模拟器并恢复配置；按驱动日志 `HELLO session=` 判定会话数。

### 根因修复：µVision 内存视图崩溃（AG_GETMEMATT 指针契约）

- 现象：B.2 首次接入后，凡打开内存/反汇编视图的调试会话必崩（UV4 stopped
  working）；关闭视图的会话正常。
- 定位：临时 `DebugMask` 逐项关闭缩小范围（`AG_MemAtt` 关闭即不崩）+ gdb 复现
  （崩溃于 `SarmCM3!HitexLoad+72081` 执行 `testl $0x80000,(%eax)`，eax=0x3）
  → 现代 µVision 把 `AG_GETMEMATT` 的返回值**当指针解引用**；官方
  `CMSIS_AGDI.dll` 反汇编确认 `attrArray + ((addr & 0xFFFF) >> 2) * 4`
  （每 64 KiB 段一块、每 4 字节一个属性 dword）。
- 修复：`AgdiMemory.cpp` 段属性块模型（32 × 64 KiB 懒分配，按真实内存映射预填
  EXEC|READ|THUMB / READ|WRITE；`AG_MEMMAP` 按官方 MapMemory 语义填充/清零、
  `ErrAdr=0`；`AG_SETMEMATT` 官方无操作）。
- 有意偏差：`AG_INITFEATURES` 恒返回 `AG_OK` ——实测返回错误码会让
  µVision 取消驱动、`-j0` 下挂起；连接失败改为"逐次访问 AG_NOACCESS + 日志"。
- 全部临时调试脚手架（`DebugMask`、TRACE 打印、bisect/gdb 脚本与日志）已在提交前删除。

### 实测验收（µVision 5.43.1.0，2026-09-27）

- 探针（无 Keil）：`BlueBridgeAGDIProbe.exe --rounds 20` → **20/20 轮 PASS**，
  每轮 69 项检查 0 失败（`build/probe_stress20.log`）。
- Keil 会话压测（固定管道）：`run_keil_session_stress.ps1 -Rounds 50
  -PipeName BlueBridgeSimulator.Debug.KeilStress` → **50/50 exit=0**、
  1.9–2.3 s/轮、+50 次 `HELLO session=`（每轮恰好一次）、0 超时/0 崩溃。
- AutoStart 路径：驱动自启模拟器（唯一管道 `…Debug.<UV4pid>.<nonce>`）→
  会话 exit=0、`LeaveSimulatorRunning=1` 保留模拟器。
- 会话内 µVision 自身就调用映射：`AG_MemAcc(AG_READ)` 读 DWT/PPB、
  `AG_RDOPC` 反汇编、`AG_MemAtt(AG_MEMMAP 0x08000000 +64 KiB attr=3)`、
  `Installed RegView: 4 groups, 64 items (FPU on)`。
- 无头回归：`ctest` **15/15 PASS ×3 轮**。

### 说明

- 本 checkpoint **不含** Run/Stop/断点/异步停止事件（B.3）与"烧录到虚拟 flash"
  的应用下载（B.4）；`AG_MemAcc(F_*)` 返回 `AG_INVALOP`。
- 验收清单：主机侧项已勾选（探针/压测/AutoStart），人工项（寄存器/内存窗口、
  Reset、单步、杀模拟器存活、双实例）待人工执行。

---

## 阶段七-2B.1 — Keil µVision AGDI 驱动（最小可加载驱动，2026-09-26）

### 环境侦察（本机实测）
- Keil MDK **5.43a**，`UV4.exe 5.43.1.0`，**32 位**（PE `IMAGE_FILE_MACHINE_I386`）
  → `BlueBridgeAGDI.dll` 必须 Win32/x86（模拟器 x64 与它通过命名管道天然跨位数）。
- 真正生效的 `TOOLS.INI` = `%LOCALAPPDATA%\Keil_v5\TOOLS.INI`；TDRV0–18 已占用；
  Cortex-M 驱动列表 = `CPUDLL1=SARMCM3.DLL(TDRV…)`；`MAXTDRV=40`（官方 COMTYP.H）。
- 官方 AGDI 取证：AppNote 173（`keil.com/appnotes/files/apnt_173.zip`）+ 官方示例包
  `keil.com/download/files/apntex_173.zip`（**AGDI.H / AGDI.CPP / COMTYP.H / SampTarg**，
  SHA-256 已在本地校验）。本机 Keil 安装树内没有
  AGDI.H / 模板，只有已编译的官方实现（CMSIS_AGDI.dll / UL2CM3.dll / JLTAgdi.dll）。

### 新增（全部在 `BlueBridgeSimulator/tools/keil_agdi/`，不参与主 CMake / ctest）
- 官方 ABI 清单整理：12 个导出原型、调用约定 = cdecl、全部 nCode / 错误码 /
  结构体 / 回调表 / 生命周期 / 注册机制，每条标注证据等级。
- `BlueBridgeAGDI/src/`：**薄驱动**（无 Qt / 无 bluesim_core / 无 unicorn）——
  `DllMain`（仅记录模块句柄 + DisableThreadLibraryCalls，不在 loader lock 下做 I/O）、
  `AgdiDriver.cpp`（`EnumUvARM7` / `DllUv3Cap` + 官方 10 个必需导出；B.1 按官方
  "未连接"语义返回：Mem/Reg → `AG_NOACCESS`，Bp/GoStep/Serial/Hist → 0，
  BreakFunc → NULL）、`AgdiLog`（`%LOCALAPPDATA%\BlueBridgeSimulator\logs\BlueBridgeAGDI.log`，
  热点路径限速、日志轮转）、`AgdiConfig`（agdi.ini，全部字段）。
- `build_agdi.ps1`：自动发现 VS（vswhere / 本机默认 / `-VsPath`）、按 **UV4 位数**选平台、
  `/MT` 静态 CRT（与官方模板一致）、构建后 **dumpbin** 校验 12 个导出名与 DLL 位数。
- `install.ps1` / `uninstall.ps1`：TOOLS.INI 备份（`*.bluebridge-<时间戳>.bak`）、
  **不覆盖**已有驱动、取最小空闲 TDRV 槽（本机 = TDRV19）、同时登记 `[ARM]`/`[ARMADS]`
  与 `CPUDLL1` 分组、可重复执行；卸载只删本驱动（不还原整份 TOOLS.INI）。
- `run_keil_session_stress.ps1` + `keil_test/`（STM32G431RBTx 工程、`exit_init.ini`
  仅含 `EXIT`）：`UV4 -d <proj> -j0 -sg` 自动化调试会话。
- 可勾选验收清单：B.1 项已勾。

### 实测验收（µVision 5.43.1.0）
- 驱动出现在 `Options for Target → Debug → Use:`（会话中状态栏显示
  `BlueBridge CT117E-M4 Simulator`）；DLL 加载 → `DllUv3Cap(2)=7` →
  `EnumUvARM7(nCode=2)=7`（`dbgblk*`）→ 官方 `AG_Init` 握手（`AG_INITITEM` × 11，
  含 `AG_INITCALLBACK` → `AG_INITFEATURES` → `AG_GETFEATURE` × 17）→ 会话 →
  `AG_UNINIT` → `DLL_PROCESS_DETACH`。
- **50/50 轮** 会话生命周期：exit = 0、每轮 2.2–2.4 s、0 崩溃、0 超时；日志统计
  50 次会话 + 100 次驱动卸载（每轮"探测 + 会话"各一次，全部配对）。
- 新观察（只记录、不猜语义）：`DllUv3Cap` 还会用 100 / 109 探测；`AG_GETFEATURE`
  出现 AN173 之外的 id（0x0E/0F/10/11/14/16–19）；`AG_EXECITEM` 出现 0x18/0x62/0x68。

### 说明
- Keil 官方文件不进入仓库（`vendor/agdi_sdk/` 已 gitignore），构建脚本仅本地引用；
  来源与摘要见 `vendor/README.md`。
- 本 checkpoint **不含**任何 IPC / 模拟器连接；B.2 才接命名管道。

---

## 阶段七-2A — Debug IPC + Virtual Flash Programming（2026-09-26）

### 新增

- **Windows 命名管道 Debug IPC 服务**（`src/debug/ipc/`，本机专用）：
  - `DebugIpcWire.h`：C 兼容 wire 定义（magic `'B''B''G''D'`、协议 1.0、
    opcode/WireStatus/寄存器 id/上限）；
  - `DebugIpcCodec`：24 字节小端 header + 手工 LE 编解码 + 边界检查 reader/writer
    （**禁止** memcpy 结构体 / JSON / `#pragma pack`）；
  - `DebugCommandQueue`：纯 C++17 线程安全队列，**控制命令优先**（HALT/RESET/STEP/
    RESUME/HELLO 不排在 memory read 之后），命令带结果槽 + abandon 标记（超时后绝无
    stale response）；
  - `DebugIpcServer`：byte mode、单实例（第二个客户端 = Busy）、`PIPE_REJECT_REMOTE_CLIENTS`，
    管道名由 `--debug-pipe` 指定（绝不写死）；worker 线程**只做 I/O + 编解码 + 入队 + 转发
    事件**，所有命令在 **Simulator owner 线程**执行（unicorn 单线程约束），断线清理也是
    入队一条内部命令（worker 不 halt、不清断点）。
- **协议能力**：HELLO（版本/会话 id/目标名/特性）、GET_CAPABILITIES（内存图取自 SoC 常量）、
  GET_STATE / GET_STOP_INFO、HALT/RESUME/STEP/RESET_HALT/RESET_RUN、PING、寄存器
  读/写/批量、READ/WRITE_MEMORY、断点增删清、`TARGET_STOPPED` / `TARGET_FAULTED`
  异步事件（真源是 Simulator 的 StopInfo，不猜）、请求超时（普通 2 s / PROGRAM_END 10 s）。
- **虚拟烧录 `IFlashProgrammer`**（`src/debug/`）：`PROGRAM_BEGIN`（要求 Halted、
  一次一事务、token 单调+会话 salt）→ `PROGRAM_ERASE`（staging 置 0xFF）→
  `PROGRAM_WRITE`（staging 覆盖，乱序/重叠允许，后写胜）→ `PROGRAM_END`（**原子 commit**：
  唯一 live flash backing + 丢弃翻译块 + reset → Halted/StopReason=Reset）→
  `PROGRAM_ABORT`（丢 staging，live flash 绝不变）；地址必须是物理 flash。
- **CLI**：`--debug-pipe <name>`、`--wait-debugger`、`--exit-on-debugger-disconnect`、
  `--debug-ipc-trace`；**不加参数时行为与以前完全一致**（无 server、无线程）。
- **GUI**：status bar 三标签（Debugger 连接/会话、Target Running/Halted、Stop 原因），
  2 ms owner-thread 命令 pump（Halted 时也跑，STEP/READ/RESET 不会卡死）。
- **测试**：`debug_ipc_selftest`（真实管道 A~W + 超时 + 20 轮稳定性）、
  `debug_program_selftest`（直测烧录 + A→B→A ×100）、`debug_ipc_codec_test`
  （24B golden bytes + 冻结枚举 `static_assert`）；
  烧录测试固件 `firmware/test_program_a|b`（同布局、0x08000042 处 opcode 0x2011 vs 0x2022）。
- **文档**：`docs/debug_ipc_protocol.md`（协议权威文档，下一阶段 AGDI 照此实现客户端）。

### 变更

- `IDebugTarget` 增加 `info()` / `virtualCycles()` / `firmwareLoaded()`；
  `DebugStatus` 增加 4 个烧录用状态（ProgramNotActive/AlreadyActive/TokenInvalid/RangeInvalid）。
- `Stm32G431` 增加 `buildFlashImage()` / `replaceFlash()`，`Simulator::loadFirmware()` 与
  烧录 commit **共用同一条"替换 flash → 丢翻译块 → reset"路径**。
- `Simulator` 增加 `readFlashImage()` / `replaceFlashImage()` / `memoryMap()` /
  `setStopObserver()` / `setWaitForDebugger()` / `setDebuggerAttached()` / `exactStepSupported()`。
- 新增 `Simulator::pauseImpl()`：只停循环不记 stop reason，内部停因（断点/故障/复位）
  不再额外发布一个假的 UserHalt 事件。

### 修复

- **会话在第一个 stop 事件后被误杀**：读循环与事件写出共用同一个 OVERLAPPED 事件，
  写完成会唤醒读等待并让 `GetOverlappedResult` 报 `ERROR_IO_INCOMPLETE`（被当成断线）。
  改为读/写**独立事件**，并把 `ERROR_IO_INCOMPLETE` 视为"继续等待"。
- **取消未回收的 overlapped I/O**：`CancelIoEx(handle,&ov)` 后未 `GetOverlappedResult(...)`
  就返回，而 `ov` 是栈上局部变量 → 内核回写已释放栈帧（表现为 `0xC0000374` 堆损坏 /
  后续 WriteFile 失败）。服务端与测试客户端所有 cancel 路径都先 reap 再返回。
- **请求超时实际 2.4 s**：超时预算用"25 ms 切片计数器"实现，而
  `std::condition_variable::wait_for()` 在 Windows 上可超时 ~15.6 ms（系统定时器粒度），
  80 个切片实测 2422 ms，导致 owner 停摆时客户端拿到迟到的 Ok 而非 Timeout。
  改为**绝对截止时间**判定（实测 `Timeout after 2012 ms`），并保留 >500 ms 的
  `[ipc] slow response` 诊断。

### 测试

- 全量 `ctest` **15/15 PASS**（其中新增 3 个；**连续 3 轮全绿**，单轮 56–66 s），
  原有 12 个测试无退化。
- `debug_ipc_selftest`：exit=0（A~W 全 PASS + **20 轮 20/20** 稳定性；含断线清理/重连、
  版本不符、坏 magic、超限 payload、请求超时恢复、运行中 64×4 KiB 流水 + HALT 优先、
  IPC 烧录 A→B→A 与事务中断线）。
- `debug_program_selftest`：exit=0（abort 不污染、token/range 错误、重叠/乱序、
  128 KiB 全镜像、**A→B→A ×100** 无 stale TCG）。
- GUI 手测：`--debug-pipe` 下真实管道跑通 HELLO/能力/状态；断线 GUI 存活；
  `--exit-on-debugger-disconnect` 干净退出；两个实例两个管道名互不串线。
- 性能实测：寄存器 RTT ≈ 33 µs、1 KiB 读 ≈ 30 µs、64×4 KiB + HALT 1–2 ms、
  128 KiB `PROGRAM_WRITE`/commit < 1 ms、100×A→B→A ≈ 12 ms、超时 2012 ms。

---

## 阶段七-2d — 精确单步 + unicorn.dll 重建（2026-09-26）

### 新增

- **unicorn 2.1.4 精确单步控制 `UC_CTL_EXACT_SINGLE_STEP`**（默认关闭，可读回）：
  - 开启后**每个翻译块恰好一条 guest 指令**（`tb_gen_code` 强制 `max_insns = 1`）；
  - 翻译结束**禁止 TB 链式跳转**（`gen_goto_tb` / `gen_goto_ptr` 改为 `exit_tb(NULL,0)`）；
  - `uc_emu_start(count=N)` 的指令预算改在 **TB 边界**按 `tb->icount` 强制执行
    （`stop_request` + `cpu_exit()`），不再依赖 count hook 的 `uc_emu_stop` 时序；
  - 内部异常（BKPT/SVC/EXC_RETURN）派发后立即停下；执行到 `icount != 1` 的块会打印一致性告警；
  - 切换该 control 自动 `tb_flush`（丢弃旧的多指令/链式翻译）。
- **BlueBridge 接线**：`ICpuEngine::setExactSingleStep()`（默认 no-op）→
  `UnicornCpu::setExactSingleStep()`（带缓存，仅真变时下发，避免每步 flush）→
  `Simulator::setExactStepMode()`，只在 `debugStepOne()` 与 `runBreakpointBatch()` 打开；
  普通 Run 走原快路径并关闭该模式。
- **新测试 `tests/exact_step_selftest.cpp`**（独立于 SoC/板级，直接用 UnicornCpu + 最小宿主）：
  A~J 十类场景**各 100000 次单步**——直线代码 / 自循环（SRAM 计数）/ taken 分支 /
  not-taken 分支 / BL·BX LR / Thumb-16 与 Thumb-2 混排 / MMIO LDR·STR /
  handler 模式（注入中断激活）/ 异常返回（EXC_RETURN magic）/ IT 块；
  另有普通批量（16 条指令一次批）与模式切换回归，以及**关闭新模式的 legacy 对照**测量。
- **真 Handler 模式启用**：重建的 `unicorn.dll` 含 `uc_arm_set_v7m_exception`
  （写 `env->v7m.exception` + `arm_rebuild_hflags`），IPSR 现在读出真实异常号，
  ISR 以真实 Handler 模式语义运行（此前是 thread 模式回退路径）。
- **JIT 可诊断性**：Windows 上 code-gen buffer 的按需提交（VEH）失败时打印
  `[uc] FATAL: cannot commit JIT page ...`（原为无提示的 0xC0000005）。

### 变更

- `debug_target_selftest` 断点压力段 **10000x → 100000x**，并把"过冲"从诊断打印升级为
  **硬失败**（每轮断言 counter +1 / cycles +3 = 恰好 3 条指令，无过冲）。
- 删除 `UnicornCpu::runBatch` 里旧的 TB_REMOVE 宿主侧缓解代码（被精确模式取代）。
- 交接文档不再纳入版本库（见下方"仓库整理"）。

### 修复

- **测试态假象纠正**：压力段历史上的"counter 0→513、cycles +3"并非单步过冲——
  `firmware/test_debug` 的循环计数放在 **r2**，而 `doReset()` 不清通用寄存器，
  前序 wall-clock 阶段把 r2 留成 512/32，导致第一次 continue 把 513/33 写进内存计数器。
  已在压力段显式初始化 `r2 = 0` 与计数器（legacy 对照实测：老 `count=1` 路径本身
  100000/100000 步精确）。
- 修复阶段七-1 遗留的 **unicorn 源码编译错误**：`uc_arm_set_v7m_exception` 的声明原写在
  `include/unicorn/arm.h`（该处 `UNICORN_EXPORT` / `uc_err` 尚未定义），移到
  `include/unicorn/unicorn.h`，否则整棵树无法编译。

### 测试

- `exact_step_selftest`：**PASS**（A~J 各 100000 步全部恰好 1 条指令；
  legacy 对照 100000/100000 与 30000/30000）。
- `debug_target_selftest`：100000x 压力**连跑 20 轮 20/20 PASS**
  （10 万次 hit/continue ≈ 305 ms，约 1.0 µs/条指令）。
- 全量 `ctest --test-dir build -C Release`：**12/12 PASS**，连续多轮稳定全绿。

### 已知限制

- **IT 块**：unicorn 把 `IT + 受控指令` 当作**一条指令单元**执行（`tb->icount == 1`，
  条件语义正确），两种模式一致 → 调试器在 IT 块内没有指令粒度（块内断点不会命中）。
- **Windows JIT 缓冲**：默认保留 1 GiB、按 4 MB 按需提交；极大量互不相同地址的翻译
  （如"10 万条不同地址各一次单步"）可能耗尽并导致提交失败（已有明确诊断打印）。
  压力测试应让代码在少量地址上循环，或调大 `UC_CTL_TCG_BUFFER_SIZE`。

---

## 阶段七-1 — Virtual Debug Target Core（2026-09-26）

### 新增

- `src/debug/` 调试抽象：`IDebugTarget`（纯 C++，无 Qt / 无 unicorn 常量）、
  `DebugTypes.h`（DebugRegister / DebugStatus / StopReason / StopInfo / TargetState）、
  `SimulatorDebugTarget`（DebugRegister→CpuReg 映射、线程断言，不持有所属对象）。
- `Simulator` 调试面：`debugStepOne()` 单步、`debugResetHalt()`、断点增删查清、
  `debugSkipCurrentBreakpointOnce()`（越过当前断点一次）、`lastStopInfo()`、
  `debugLogLine()`。
- **精确断点（方案 B）**：`Simulator::runBreakpointBatch()` —— 宿主机侧在每条指令前
  检查断点集，每批只跑 1 条指令，保证"停在断点地址且该指令未执行"。
- 测试固件 `firmware/test_debug/`（固定布局 Thumb 汇编 + `0x08000200` debug map）。
- 测试 `tests/debug_target_selftest.cpp`（Test A~U + 断点压力段）。

### 变更

- `ICpuEngine` 扩展：寄存器/内存读写、单步、`executedLastBatch()` 等调试所需接口。

### 测试

- `debug_target_selftest`：PASS；`ctest` 11/11。

---

## 阶段六 — 恒定 1:1 实时 + 换固件修复 + 手测固件 + 发布包（2026-09-26）

### 新增

- 综合手测固件 `firmware/test_full/`（流水灯 / 3 页屏测 / 按键翻页 / PA7 PWM 档位 /
  PA15·PB4 捕获 / R37·R38 ADC 显示）。
- 发布包 `BlueBridgeSimulator/release/`（免安装：bluesim.exe + Qt 运行库 + unicorn.dll +
  `firmware/*.hex`）。

### 变更

- **恒定 1:1 实时**：删除速度档位（GUI 下拉框 / `Simulator::setSpeed()` / `speedX_`），
  工具栏显示 `Real time 1:1`；虚拟时间严格不超前于墙钟。
- 配速基准改为"本次运行起点"（`startRun()` 记录 `runStartCycles_`）。

### 修复

- **换固件后 Run 无反应**：1:1 配速原用绝对虚拟时间与墙钟比较，Reset / 换固件后
  `cycles_` 未重置导致虚拟时间远大于墙钟；改为按本次运行差值比较。
- `loadFirmware()` 运行中不再静默拒绝：自动暂停后加载并记录
  `[sim] auto-paused to load a new firmware`。
- `rebuildCpu()` 中 flush 翻译块，避免换固件后执行到旧代码。

### 测试

- 新增 `tests/reload_selftest.cpp`（Open→Run→Reset→换固件→Run）与
  `tests/full_selftest.cpp`；`ctest` 10/10。

---

## 阶段五 — USART1 + 虚拟串口终端（2026-09-26）

### 新增

- USART 模型 `src/stm32/usart/Usart.{h,cpp}`：USART1 基址 `0x40013800`、IRQn=37、
  门控 `APB2ENR.USART1EN`；寄存器 CR1/CR2/CR3、BRR、PRESC、ISR、ICR、RDR、TDR、RQR；
  标志 RXNE(RXFNE)/TC/TXE(TXFNF)/ORE 真实置清；RXNEIE/TCIE/TXEIE 生效。
- 接线：PA9=USART1_TX、PA10=USART1_RX（AF7），与 TIM 共用同一套 AF 路由。
- 虚拟串口对端 `src/board/serial/VirtualSerialPeer.h`：按 baud 排帧（9600 / 115200 实测），
  GUI "Serial" 页（终端显示 + 输入框 + 默认勾选的追加 CR/LF）。
- 测试固件 `firmware/test_usart/` 与测试 `tests/usart_selftest.cpp`。

### 修复

- **115200 吞吐退化**：发射器"已使能但空闲"时未按帧长切片，字节被推迟到批尾
  （100 字节 25 ms → 修复后 8.7 ms）。

### 测试

- `usart_selftest`：寄存器 / 双 baud 字节级时序 / TX / RX / 中断 / ORE / 行协议；`ctest` 8/8。

---

## 阶段四 — PA15/PB4 脉冲输入 + PA7 PWM 监测 + R37/R38 ADC（2026-09-26）

### 新增

- 板级虚拟信号源 `PulseInputSource`（PA15/PB4，1 Hz–100 kHz，占空比 1–99%）；
- pin 波形监视器 `DigitalSignalMonitor`（PA7 实测频率/占空比）；
- 电位器电压源 `AnalogSource`（R37/R38）+ ADC 模型 `Adc.{h,cpp}`（HAL 兼容转换序列、
  分辨率/对齐、EOC/OVR 语义）；
- GUI Signals 页 4 个旋钮（与固件/模型双向同步）；
- 测试固件 `firmware/test_adc_pwm/` 与测试 `tests/analog_selftest.cpp`。

### 变更

- 旋钮范围等板级参数集中到 `src/board/BoardProfile.h`。

### 测试

- `analog_selftest`：PASS（7/7 回归）。

---

## 阶段三 — TIM 定时器真实仿真（2026-09-25 夜）

### 新增

- TIM1/2/3/4/6/7/8/15/16/17 模型（基址 `0x40000000`/`0x40000400`/`0x40000800`/
  `0x40001000`/`0x40001400`/`0x40012C00`/`0x40013400`/`0x40014000`/`0x40014400`/`0x40014800`）。
- 寄存器：CR1（CEN/UDIS/URS/DIR/CMS/ARPE）、CR2、SMCR、DIER、SR、EGR（UG/CCxG）、
  CCMR1/2（CCxS/OCxM/OCxPE）、CCER（CCxE/CCxP/CCxNP）、CNT、PSC（缓冲）、ARR（ARPE）、
  CCR1–4（预装载/捕获锁存）、BDTR（MOE）、DCR/DMAR、AF1/AF2、TISEL。
- 中断映射：TIM2=28、TIM3=29、TIM4=30、TIM6=54、TIM7=55、TIM1=25、TIM8=44、
  TIM15=24、TIM16=25、TIM17=26（经 NVIC `pend(16+n)` 注入真实异常）。
- 测试固件 `firmware/test_tim/`（裸机 C，全 MMIO）与 `tests/tim_selftest.cpp`。

### 修复

- AF 输出"首 pin 优先"导致 PWM 输出到 PA4；
- 捕获通道 1-based/0-based 索引错位；
- 信号监视器重启时遗漏 stale 上升沿；
- **`ARR < CNT` 动态调频 bug**：运行中改小 ARR 时 wrap/compare 距离下溢，导致计数器异常、
  PWM 边沿停止（按"计到计数器位宽自然溢出"修复）。

### 测试

- `tim_selftest`：1 kHz 更新中断、PA7 实测 1000 Hz/50%、动态占空比、1k→2k 捕获、
  溢出捕获、错 AF 无波形、CEN=0 不跑、性能段。

---

## 阶段二 — LCD（ILI9325 总线级仿真）（2026-09-25 末）

### 新增

- LCD 控制器模型 `LcdController`：器件码 `0x9325`（走 REG_932X_Init 路径）、
  命令 0x00/0x03/0x07/0x20/0x21/0x22/0x50–0x53、GRAM 240×320、窗口、AM/I-D 与
  "面板横向 320×240 ↔ GRAM 纵向 240×320"的映射（`px = 319 - gramY, py = gramX`）。
- 接线：GPIOC[15:0]=DB[15:0]（16 位 RGB565）、PB5=WR（下降沿锁存）、PB8=RS、
  PB9=NCS、PA8=NRD、LCD_RST 随板级复位。
- GUI `LcdWidget`（RGB565→QImage、按控件等比铺满、最近邻采样保持像素锐利）。
- 测试固件 `firmware/test_lcd/`（官方 BSP 原样）与 `tests/lcd_selftest.cpp`
  （A 总线/GRAM 模型 + B 固件端到端 + C Qt 控件离屏渲染）。

### 变更

- 未实现的 LCD 命令按 accept+store 处理（无行为影响，每个索引只告警一次）。

### 修复

- 官方 BSP 逐像素 GPIO 总线刷新导致屏幕卡顿 → 分行缓存（只重画内容变化的行），
  像素写入量 ~107k/秒 → ~13k/秒；
- GRAM↔面板映射错误导致的转置/镜像（固件侧加 `rect_panel`/`circle_panel`/`fill_panel`
  坐标包装）。

### 已加护栏

- LCD 跟踪日志每次 reset 后最多 2000 行（防止 GUI 线程被日志卡死，超出后静默并提示）。

---

## 阶段一 — 核心框架（2026-09-25）

### 新增

- 四层架构：`CPU（ICpuEngine → UnicornCpu → unicorn.dll）` / `STM32（Stm32G431 + 外设）` /
  `Board（Ct117eM4 + 虚拟器件）` / `GUI（Qt 6）`；
- `unicorn.dll` 运行库动态加载（`UnicornApi`，GetProcAddress，可选符号回退）；
- 存储/总线：Flash 128 KB + SRAM 32 KB + CCM 16 KB 映射（`uc_mem_map_ptr` 共享后备缓冲）、
  MMIO 钩子（外设窗口由宿主模型应答）；
- GPIO（MODER/OTYPER/OSPEEDR/PUPDR/IDR/ODR/BSRR/AFR）、RCC（时钟树/门控/复位）、
  PWR、FLASH、EXTI 桩、PPB（SysTick / NVIC / SCB）、虚拟按键/LED 锁存；
- Intel HEX / BIN 加载器与 `tests/test_hexloader.cpp`；
- GUI：LCD 页、LED/按键、Signals 页、Serial 页、日志面板、性能诊断
  （`[perf]` 行：实时倍率、批切片来源、MMIO 速率）。

### 修复（后续沉淀）

- **手动 NVIC 中断注入方案**：unicorn 把 `< EXCP_INTERRUPT` 的 CPU 异常统一路由到
  `UC_HOOK_INTR`（不走 `do_interrupt`），且 `v7m_msr_xpsr` 只允许写 APSR →
  异常入口由 `Simulator::doExceptionEntry` 手动压帧/取向量，出口由
  `hookIntr(8)` → `UnicornCpu::popExceptionFrame()` 真实弹帧（阶段七-2d 起
  `setV7mException` 让 Handler 模式成为真状态）。
- 时间推进护栏：`advanceTime()` 钳制 delta ≤ 16.7M 周期（超限落盘告警），
  SysTick/Timer 推进循环加独立迭代上限；
- 按键采样移入 1 ms SysTick 中断（8 ms 去抖），动作由主循环消费，
  撤掉 LCD 重绘期间的 `cpsid i`（避免 25–35 ms 内点击丢失）；
- GUI 启动崩溃（PATH 里旧 Qt6Widgets.dll 抢加载）→ windeployqt 部署运行库到 exe 目录。

---

## 仓库整理

- 内部交接文档 `PROJECT_HANDOFF.md` **不再纳入版本库**（加入 `.gitignore`，
  仅保留在本地工作副本，供后续开发/接手使用）。