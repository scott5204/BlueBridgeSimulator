CT117E-M4 / STM32G431RBT6 全软件模拟器（免安装）
================================================

运行：双击 bluesim.exe（无需 Qt、无需工具链、无需驱动）

  1. Open     载入 firmware\ 下的任意 .hex
  2. Run      开始仿真（恒定 1:1 实时：delay(1000) 就是 1 秒真实时间，无速度档位）
  3. 页签     LCD / Signals / Serial / Log
              Signals 页：PA15、PB4 两个外部信号发生器（频率可调）+ R37/R38 两个电位器
              Serial  页：虚拟串口终端（PC <-> MCU）

本目录自带 Qt6 与 unicorn 运行库（x64），整个文件夹拷到任意 Windows 10/11 即可运行。
运行日志写在同目录 bluesim.log（每行即时落盘，界面卡住时也能取到现场）。

命令行（可选，供调试器接入；不加参数时行为与以前完全一致）
------------------------------------------------------------
  bluesim.exe --debug-pipe <名字>          打开 Debug IPC 命名管道服务
                                            （例：BlueBridgeSimulator.Debug.1234.A1B2）
  bluesim.exe --debug-pipe <名字> --wait-debugger
                                            未连接调试器前保持 Halted（不自动 Run）
  bluesim.exe --debug-pipe <名字> --exit-on-debugger-disconnect
                                            调试器断开后自动退出（默认继续运行）
  bluesim.exe --debug-pipe <名字> --debug-ipc-trace
                                            每收发包写一行日志（默认关闭）
  协议：仓库 docs\debug_ipc_protocol.md（v1.0，24 字节小端头 + 命名管道）

firmware\ 内的测试固件
----------------------
  test_full.hex      综合自测：流水灯 + 3 页屏幕测试 + B1~B4 按键 + PA7 PWM 输出
                     + PA15/PB4 捕获测频 + R37/R38 电压显示
                     按键：B1 流水灯方向 / B2 速度 / B3 PWM 档位 / B4 翻页
  test_usart.hex     串口（9600 8N1）：发 (48,92) -> "Got it"，? -> "Idle"，# -> 坐标
  test_adc_pwm.hex   省赛闭环：电位器控制 PWM 频率/占空比，输入频率比对后报警
  test_lcd.hex       LCD 控制器 / 官方 BSP 总线级测试
  test_blink.hex     最小点灯 + 按键
  test_tim.hex       定时器 PWM / 输入捕获
  test_tim_demo.hex  0.5 s 心跳灯 + 1 kHz 75% PWM + LCD
  test_debug.hex     调试目标测试程序（固定布局 Thumb 汇编，供无头 debug 验收使用）
  test_program_a.hex / test_program_b.hex
                     虚拟烧录测试固件（同布局、不同 opcode，验证烧录后无旧代码残留）

建议先跑 test_full.hex，再用 Signals 页的信号发生器 / 电位器交互。