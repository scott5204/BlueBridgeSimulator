#include "gui/MainWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDial>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

#include "debug/SimulatorDebugTarget.h"
#include "debug/SimulatorFlashProgrammer.h"
#include "debug/ipc/DebugIpcServer.h"
#include "gui/LcdWidget.h"
#include "gui/LedIndicator.h"

namespace {
// stop bits as CR2.STOP codes them (half-bit units: 2 = one stop bit)
QString stopBitsText(uint32_t half) {
    switch (half) {
    case 1: return "0.5";
    case 2: return "1";
    case 3: return "1.5";
    default: return "2";
    }
}
}  // namespace

MainWindow::MainWindow(const DebugIpcUiConfig& ipc, QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(tr("BlueBridge Simulator - CT117E-M4 (STM32G431RBT6)"));
    setAcceptDrops(true);
    resize(1180, 720);

    // ---- toolbar -----------------------------------------------------------
    QToolBar* bar = addToolBar(tr("Main"));
    bar->setMovable(false);

    QAction* openAct = bar->addAction(tr("Open HEX/BIN..."));
    runBtn_ = new QPushButton(tr("Run"));
    pauseBtn_ = new QPushButton(tr("Pause"));
    resetBtn_ = new QPushButton(tr("Reset"));
    stepBtn_ = new QPushButton(tr("Step"));
    // No speed selector: the simulation is always 1:1 with real time (see
    // Simulator::runTick), which is what a competition firmware expects --
    // delay(1000) is one second.

    for (QWidget* w : {runBtn_, pauseBtn_, resetBtn_, stepBtn_})
        bar->addWidget(w);
    bar->addWidget(new QLabel(tr("  Real time 1:1")));
    fwLabel_ = new QLabel(tr("no firmware"));
    statusBar()->addPermanentWidget(fwLabel_);

    runBtn_->setEnabled(false);
    pauseBtn_->setEnabled(false);
    resetBtn_->setEnabled(false);
    stepBtn_->setEnabled(false);

    connect(openAct, &QAction::triggered, this, &MainWindow::openFirmware);
    connect(runBtn_, &QPushButton::clicked, this, &MainWindow::onRunClicked);
    connect(pauseBtn_, &QPushButton::clicked, this, &MainWindow::onPauseClicked);
    connect(resetBtn_, &QPushButton::clicked, this, &MainWindow::onResetClicked);
    connect(stepBtn_, &QPushButton::clicked, this, &MainWindow::onStepClicked);

    // ---- central layout ----------------------------------------------------
    QWidget* central = new QWidget(this);
    QHBoxLayout* lay = new QHBoxLayout(central);
    lay->addWidget(buildBoardPanel(), 3);
    QTabWidget* tabs = new QTabWidget();
    tabs->addTab(buildCpuPanel(), tr("CPU"));
    tabs->addTab(buildPeriphPanel(), tr("Peripherals"));
    tabs->addTab(buildSignalPanel(), tr("Signals"));
    tabs->addTab(buildSerialPanel(), tr("Serial"));
    QWidget* logTab = new QWidget();
    QVBoxLayout* logLay = new QVBoxLayout(logTab);
    logView_ = new QPlainTextEdit();
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    logView_->setFont(mono);
    logLay->addWidget(logView_);
    tabs->addTab(logTab, tr("Log"));
    lay->addWidget(tabs, 2);
    setCentralWidget(central);

    // ---- simulator wiring --------------------------------------------------
    connect(&sim_, &Simulator::stateChanged, this, &MainWindow::onStateChanged,
            Qt::QueuedConnection);
    connect(&sim_, &Simulator::runningChanged, this,
            &MainWindow::onRunningChanged, Qt::QueuedConnection);
    connect(&sim_, &Simulator::logMessage, this, &MainWindow::onLog,
            Qt::QueuedConnection);
    connect(&sim_, &Simulator::firmwareLoadedSig, this,
            &MainWindow::onFirmwareLoaded, Qt::QueuedConnection);
    connect(&sim_, &Simulator::firmwareLoadFailed, this,
            &MainWindow::onFirmwareLoadFailed, Qt::QueuedConnection);
    connect(&sim_, &Simulator::faulted, this, &MainWindow::onFaulted,
            Qt::QueuedConnection);

    // ---- stage 7-2A: Debug IPC (only with --debug-pipe) --------------------
    if (ipc.enabled) setupDebugIpc(ipc);
}

MainWindow::~MainWindow() {
    if (ipcServer_) {
        // Stop the pump and detach the stop observer BEFORE the members go
        // away (the Simulator destructor pauses, which would otherwise call
        // back into a half-destroyed window / server).
        if (ipcTimer_) ipcTimer_->stop();
        sim_.setStopObserver(nullptr);
        sim_.setWaitForDebugger(false);
        ipcServer_->stop();
    }
}

// ============================================================================
// Stage 7-2A Debug IPC front-end.
//
//   Named Pipe (worker thread) -> DebugCommandQueue -> onIpcTick() on THIS
//   thread -> SimulatorDebugTarget / SimulatorFlashProgrammer -> Simulator.
//
// The pump runs on a 2 ms QTimer regardless of the target state, so a debugger
// that halted the target can still step / read / reset it (spec 39/40). The
// GUI itself only displays status (spec 113) -- it implements no debugger UI.
// ============================================================================
void MainWindow::setupDebugIpc(const DebugIpcUiConfig& cfg) {
    DebugIpcServer::Options opts;
    opts.pipeName = cfg.pipeName;
    opts.trace = cfg.trace;
    debugTarget_ = std::make_unique<SimulatorDebugTarget>(sim_);
    flashProgrammer_ = std::make_unique<SimulatorFlashProgrammer>(sim_);
    ipcServer_ = std::make_unique<DebugIpcServer>(opts);
    // [ipc]/[program] log lines are queued by the worker and drained on this
    // thread inside processPendingCommands, so the session logger stays
    // single-threaded.
    ipcServer_->setLogSink([this](const std::string& s) {
        sim_.debugLog(QString::fromStdString(s));
    });
    ipcServer_->setAttachObserver(
        [this](bool on) { sim_.setDebuggerAttached(on); });
    sim_.setStopObserver([this](const StopInfo& info, uint64_t cycles) {
        if (ipcServer_) ipcServer_->notifyTargetStopped(info, cycles);
    });
    sim_.setWaitForDebugger(cfg.waitDebugger);
    ipcExitOnDisconnect_ = cfg.exitOnDisconnect;

    ipcStatusVal_ = new QLabel(tr("Debugger: not connected"));
    ipcTargetVal_ = new QLabel(tr("Target: -"));
    ipcStopVal_ = new QLabel(tr("Stop: -"));
    statusBar()->addPermanentWidget(ipcStatusVal_);
    statusBar()->addPermanentWidget(ipcTargetVal_);
    statusBar()->addPermanentWidget(ipcStopVal_);

    ipcTimer_ = new QTimer(this);
    ipcTimer_->setTimerType(Qt::PreciseTimer);
    ipcTimer_->setInterval(2);
    connect(ipcTimer_, &QTimer::timeout, this, &MainWindow::onIpcTick);
    ipcTimer_->start();

    if (cfg.waitDebugger) {
        sim_.debugLog("[ipc] --wait-debugger: the target stays Halted until a "
                      "debugger session connects");
    }
    ipcServer_->start();  // a creation failure is logged as [ipc] Failed ...
}

void MainWindow::onIpcTick() {
    if (!ipcServer_ || !debugTarget_ || !flashProgrammer_) return;
    // 1) execute queued debugger commands on the owner thread (bounded work,
    //    control commands first; safe and required while Halted)
    ipcServer_->processPendingCommands(*debugTarget_, *flashProgrammer_);

    // 2) lightweight status display (repainted only when something changed)
    const bool connected = ipcServer_->clientConnected();
    const QString status = connected
        ? tr("Debugger: connected (session %1)")
              .arg(ipcServer_->sessionId() & 0xFFFFFFFFull, 8, 16, QChar('0'))
        : tr("Debugger: not connected");
    if (status != ipcStatusText_) {
        ipcStatusText_ = status;
        ipcStatusVal_->setText(status);
    }
    const QString targetText =
        sim_.isRunning() ? tr("Target: Running") : tr("Target: Halted");
    if (targetText != ipcTargetText_) {
        ipcTargetText_ = targetText;
        ipcTargetVal_->setText(targetText);
    }
    const StopInfo si = sim_.lastStopInfo();
    const QString stopText =
        si.reason == StopReason::None
            ? tr("Stop: -")
            : tr("Stop: %1 @0x%2")
                  .arg(QString::fromLatin1(debug::toString(si.reason)))
                  .arg(si.pc, 8, 16, QChar('0'));
    if (stopText != ipcStopText_) {
        ipcStopText_ = stopText;
        ipcStopVal_->setText(stopText);
    }

    if (ipcWasConnected_ && !connected && ipcExitOnDisconnect_) {
        sim_.debugLog("[ipc] debugger disconnected -> exiting "
                      "(--exit-on-debugger-disconnect)");
        QCoreApplication::quit();
    }
    ipcWasConnected_ = connected;
}

QWidget* MainWindow::buildBoardPanel() {
    QWidget* panel = new QWidget();
    QVBoxLayout* lay = new QVBoxLayout(panel);

    // LCD panel: real ILI9325 GRAM content (320x240 landscape as on the board)
    QGroupBox* lcdBox = new QGroupBox(tr("LCD 320x240 (ILI9325, GPIOC data + "
                                         "PB5 WR + PB8 RS + PB9 NCS + PA8 RD)"));
    QVBoxLayout* lcdLay = new QVBoxLayout(lcdBox);
    lcd_ = new LcdWidget();
    lcdLay->addWidget(lcd_, 1);
    lay->addWidget(lcdBox, 3);

    // LED bank
    QGroupBox* ledBox = new QGroupBox(tr("LED (PC8-PC15, low active, "
                                         "74LS573 latch, LE=PD2)"));
    QHBoxLayout* ledLay = new QHBoxLayout(ledBox);
    for (int i = 0; i < 8; i++) {
        leds_[i] = new LedIndicator(QString("LD%1").arg(i + 1));
        ledLay->addWidget(leds_[i]);
    }
    lay->addWidget(ledBox);

    // Buttons
    QGroupBox* keyBox = new QGroupBox(
        tr("Buttons (B1=PB0 B2=PB1 B3=PB2 B4=PA0, low active)"));
    QHBoxLayout* keyLay = new QHBoxLayout(keyBox);
    for (int i = 0; i < 4; i++) {
        keyBtns_[i] = new QPushButton(QString("B%1").arg(i + 1));
        keyBtns_[i]->setMinimumHeight(44);
        keyBtns_[i]->setCheckable(false);
        keyLay->addWidget(keyBtns_[i]);
        const int idx = i;
        connect(keyBtns_[i], &QPushButton::pressed, this,
                [this, idx] { sim_.setButton(idx, true); });
        connect(keyBtns_[i], &QPushButton::released, this,
                [this, idx] { sim_.setButton(idx, false); });
    }
    lay->addWidget(keyBox);

    // board title
    QLabel* title = new QLabel(
        tr("CT117E-M4 - STM32G431RBT6 @ 80 MHz - HSE 24 MHz"));
    title->setAlignment(Qt::AlignCenter);
    lay->addWidget(title);
    return panel;
}

QWidget* MainWindow::buildCpuPanel() {
    QWidget* panel = new QWidget();
    QFormLayout* form = new QFormLayout(panel);
    auto mkVal = [this](const QString& init) {
        QLabel* l = new QLabel(init);
        QFont mono("Consolas");
        mono.setStyleHint(QFont::Monospace);
        l->setFont(mono);
        return l;
    };
    pcVal_ = mkVal("0x00000000");
    spVal_ = mkVal("0x00000000");
    lrVal_ = mkVal("0x00000000");
    xpsrVal_ = mkVal("0x00000000");
    ipsrVal_ = mkVal("0x00000000");
    cyclesVal_ = mkVal("0");
    clockVal_ = mkVal("16 MHz (HSI)");
    excVal_ = mkVal("thread");
    form->addRow("PC", pcVal_);
    form->addRow("SP", spVal_);
    form->addRow("LR", lrVal_);
    form->addRow("xPSR", xpsrVal_);
    form->addRow("IPSR", ipsrVal_);
    for (int i = 0; i < 13; i++) {
        regVals_[i] = mkVal("0x00000000");
        form->addRow(QString("R%1").arg(i), regVals_[i]);
    }
    form->addRow("Cycles", cyclesVal_);
    form->addRow("SYSCLK", clockVal_);
    form->addRow("Exception", excVal_);
    return panel;
}

QWidget* MainWindow::buildPeriphPanel() {
    QWidget* panel = new QWidget();
    QFormLayout* form = new QFormLayout(panel);
    auto mkVal = [](const QString& init) {
        QLabel* l = new QLabel(init);
        QFont mono("Consolas");
        mono.setStyleHint(QFont::Monospace);
        l->setFont(mono);
        return l;
    };
    systickVal_ = mkVal("CSR=0 RVR=0 CVR=0");
    gpioCVal_ = mkVal("ODR=0x0000 IDR=0x0000");
    gpioABVal_ = mkVal("GPIOA.IDR=0x0000 GPIOB.IDR=0x0000");
    rccVal_ = mkVal("CR=0x00000063 CFGR=0x00000000");
    lcdVal_ = mkVal("cmds=0 pixels=0 last=0x0000");
    form->addRow("SysTick", systickVal_);
    form->addRow("GPIOC", gpioCVal_);
    form->addRow("GPIOA/B", gpioABVal_);
    form->addRow("RCC", rccVal_);
    form->addRow("LCD", lcdVal_);
    return panel;
}

// Signal / Analog panel (stage 4): the pulse input knob, the two
// potentiometers and the PWM output measurement.
//
// Pin-level / board-level only:
//   * PA15 pulse input: the GUI writes the BOARD pulse source, which produces
//     real digital edges on the pin; the firmware still has to configure the
//     AF and the timer capture to measure anything.
//   * R37 / R38: the GUI writes the BOARD analog source; the voltage reaches
//     the firmware only through the ADC conversion of the wired channel.
//   * PA7 output: READ-ONLY. Frequency/duty come from the pin waveform monitor
//     (never from PSC/ARR/CCR), so a wrong AF configuration really shows
//     "no waveform / INACTIVE".
// All knob ranges come from the board profile, never from GUI constants.
QWidget* MainWindow::buildSignalPanel() {
    QWidget* panel = new QWidget();
    QVBoxLayout* lay = new QVBoxLayout(panel);
    auto mkVal = [](const QString& init) {
        QLabel* l = new QLabel(init);
        QFont mono("Consolas");
        mono.setStyleHint(QFont::Monospace);
        l->setFont(mono);
        return l;
    };
    const BoardProfile prof = sim_.boardProfile();

    // ---- PA15 / PB4 pulse inputs (the "555" replacement, two sources) ----
    auto mkPulseBox = [&](const QString& title, QCheckBox*& enable,
                          QDial*& dial, QSpinBox*& spin, QLabel*& state,
                          bool isPb4) {
        QGroupBox* box = new QGroupBox(title);
        QGridLayout* g = new QGridLayout(box);
        enable = new QCheckBox(tr("Enabled (drives the pin)"));
        dial = new QDial();
        dial->setRange(int(prof.pulseMinHz), int(prof.pulseMaxHz));
        dial->setValue(1000);
        dial->setNotchesVisible(true);
        dial->setMinimumSize(90, 90);
        spin = new QSpinBox();
        spin->setRange(int(prof.pulseMinHz), int(prof.pulseMaxHz));
        spin->setValue(1000);
        spin->setSingleStep(10);
        spin->setSuffix(" Hz");
        state = mkVal("off");
        g->addWidget(enable, 0, 0, 1, 2);
        g->addWidget(dial, 1, 0, 2, 1);
        g->addWidget(spin, 1, 1);
        g->addWidget(new QLabel(tr("Frequency (400 Hz .. 20 kHz)")), 2, 1);
        g->addWidget(state, 3, 0, 1, 2);
        lay->addWidget(box);

        // dial <-> spin stay in sync; both push to the board source
        connect(dial, &QDial::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this, dial, isPb4](int hz) {
                    if (dial->value() != hz) dial->setValue(hz);
                    if (isPb4) sim_.setPb4PulseFrequency(double(hz));
                    else sim_.setPulseFrequency(double(hz));
                });
        connect(enable, &QCheckBox::toggled, this,
                [this, spin, isPb4](bool on) {
                    if (on) {
                        if (isPb4) sim_.setPb4PulseFrequency(double(spin->value()));
                        else sim_.setPulseFrequency(double(spin->value()));
                    }
                    if (isPb4) sim_.setPb4PulseEnabled(on);
                    else sim_.setPulseEnabled(on);
                });
    };
    mkPulseBox(tr("PA15 pulse input (TIM2_CH1) - external pulse source"),
               pulseEnable_, pulseDial_, pulseSpin_, pulseStateVal_, false);
    mkPulseBox(tr("PB4 pulse input (TIM16_CH1 AF1) - second pulse source"),
               pb4Enable_, pb4Dial_, pb4Spin_, pb4StateVal_, true);

    // ---- R37 / R38 potentiometers ----
    QGroupBox* analogBox =
        new QGroupBox(tr("Analog inputs - R37 (ADC2_IN15/PB15) and "
                         "R38 (ADC1_IN11/PB12)"));
    QGridLayout* analogLay = new QGridLayout(analogBox);
    const int steps = int((prof.analogMaxVoltage - prof.analogMinVoltage) * 100);
    auto mkAnalogRow = [&](int row, const QString& name, QDial*& dial,
                           QDoubleSpinBox*& spin, QLabel*& val) {
        analogLay->addWidget(new QLabel(name), row, 0);
        dial = new QDial();
        dial->setRange(0, steps);
        dial->setValue(0);
        dial->setNotchesVisible(true);
        dial->setMinimumSize(90, 90);
        spin = new QDoubleSpinBox();
        spin->setRange(prof.analogMinVoltage, prof.analogMaxVoltage);
        spin->setSingleStep(0.01);
        spin->setDecimals(2);
        spin->setSuffix(" V");
        val = mkVal("0.00 V");
        analogLay->addWidget(dial, row, 1, 2, 1);
        analogLay->addWidget(spin, row, 2);
        analogLay->addWidget(val, row + 1, 2);
        return row + 2;
    };
    int r = 0;
    r = mkAnalogRow(r, tr("R37 / Analog A"), r37Dial_, r37Spin_, r37Val_);
    r = mkAnalogRow(r, tr("R38 / Analog B"), r38Dial_, r38Spin_, r38Val_);
    (void)r;
    lay->addWidget(analogBox);

    // ---- PA7 PWM output (read-only) ----
    QGroupBox* outBox = new QGroupBox(
        tr("PA7 PWM output (TIM3_CH2 / TIM17_CH1) - measured at the pin"));
    QFormLayout* outForm = new QFormLayout(outBox);
    pa7StatusVal_ = mkVal("INACTIVE");
    pa7FreqVal_ = mkVal("0 Hz");
    pa7DutyVal_ = mkVal("--");
    pa7LevelVal_ = mkVal("low");
    pa7EdgesVal_ = mkVal("0");
    outForm->addRow("Status", pa7StatusVal_);
    outForm->addRow("Frequency", pa7FreqVal_);
    outForm->addRow("Duty", pa7DutyVal_);
    outForm->addRow("Level", pa7LevelVal_);
    outForm->addRow("Edges", pa7EdgesVal_);
    lay->addWidget(outBox);
    lay->addStretch(1);

    // ---- wiring for the two potentiometers (dial <-> spin, push to board) ----
    auto wireAnalog = [this](QDial* dial, QDoubleSpinBox* spin, bool isR37) {
        connect(dial, &QDial::valueChanged, this,
                [spin, isR37, this](int v) {
                    const double volts = double(v) / 100.0;
                    if (std::abs(spin->value() - volts) > 0.001) {
                        spin->setValue(volts);
                    }
                    if (isR37) sim_.setR37Voltage(volts);
                    else sim_.setR38Voltage(volts);
                });
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [dial, isR37, this](double volts) {
                    const int v = int(volts * 100.0 + 0.5);
                    if (dial->value() != v) dial->setValue(v);
                    if (isR37) sim_.setR37Voltage(volts);
                    else sim_.setR38Voltage(volts);
                });
    };
    wireAnalog(r37Dial_, r37Spin_, true);
    wireAnalog(r38Dial_, r38Spin_, false);
    return panel;
}

// ---------------------------------------------------------------------------
// Serial tab (stage 5): the virtual PC terminal of the board's DAP-Link USB
// serial port (USART1, PA9 TX / PA10 RX, AF7).
//
// The GUI may only TYPE bytes (they enter the board's serial peer and reach the
// firmware through the real USART1 RX path) and DISPLAY what the firmware
// transmitted (read from the peer). Every status row comes from the USART
// model, so what is shown is the firmware's own configuration -- the GUI never
// sets a baud rate or touches a register.
// ---------------------------------------------------------------------------
QWidget* MainWindow::buildSerialPanel() {
    QWidget* panel = new QWidget();
    QVBoxLayout* lay = new QVBoxLayout(panel);
    auto mkVal = [](const QString& init) {
        QLabel* l = new QLabel(init);
        QFont mono("Consolas");
        mono.setStyleHint(QFont::Monospace);
        l->setFont(mono);
        return l;
    };

    QGroupBox* statusBox =
        new QGroupBox(tr("USART1 - DAP-Link serial port (PA9 TX / PA10 RX, "
                         "AF7), configured by the firmware"));
    QFormLayout* form = new QFormLayout(statusBox);
    serialStatusVal_ = mkVal("disabled (USART1EN=0)");
    serialBaudVal_ = mkVal("--");
    serialFormatVal_ = mkVal("--");
    serialFlagsVal_ = mkVal("RXNE=0 TC=0 ORE=0");
    serialCountersVal_ = mkVal("MCU->PC 0  PC->MCU 0  (queued 0)");
    form->addRow(tr("Status"), serialStatusVal_);
    form->addRow(tr("Baud"), serialBaudVal_);
    form->addRow(tr("Format"), serialFormatVal_);
    form->addRow(tr("Flags"), serialFlagsVal_);
    form->addRow(tr("Traffic"), serialCountersVal_);
    lay->addWidget(statusBox);

    QGroupBox* termBox =
        new QGroupBox(tr("Terminal   (> PC -> MCU,   < MCU -> PC)"));
    QVBoxLayout* termLay = new QVBoxLayout(termBox);
    serialTerminal_ = new QPlainTextEdit();
    serialTerminal_->setReadOnly(true);
    serialTerminal_->setMaximumBlockCount(2000);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    serialTerminal_->setFont(mono);
    termLay->addWidget(serialTerminal_, 1);

    QHBoxLayout* inputLay = new QHBoxLayout();
    serialInput_ = new QLineEdit();
    serialInput_->setPlaceholderText(tr("type a line and press Enter"));
    serialInput_->setFont(mono);
    QPushButton* sendBtn = new QPushButton(tr("Send"));
    serialCrLf_ = new QCheckBox(tr("Append CR/LF"));
    // on by default: the line protocol of the usual competition firmware (and
    // of test_usart) completes a command on CR/LF
    serialCrLf_->setChecked(true);
    QPushButton* clearBtn = new QPushButton(tr("Clear"));
    inputLay->addWidget(serialInput_, 1);
    inputLay->addWidget(sendBtn);
    inputLay->addWidget(serialCrLf_);
    inputLay->addWidget(clearBtn);
    termLay->addLayout(inputLay);
    lay->addWidget(termBox, 1);

    connect(sendBtn, &QPushButton::clicked, this, &MainWindow::onSerialSend);
    connect(serialInput_, &QLineEdit::returnPressed, this,
            &MainWindow::onSerialSend);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onSerialClear);
    return panel;
}

// ---------------------------------------------------------------------------
// serial terminal helpers
// ---------------------------------------------------------------------------
void MainWindow::appendSerialLine(const QString& prefix, const QString& text) {
    QTextCursor c(serialTerminal_->document());
    c.movePosition(QTextCursor::End);
    if (c.position() > 0) c.insertText("\n");
    c.insertText(prefix + text);
    serialTerminal_->setTextCursor(c);
    serialTerminal_->ensureCursorVisible();
    serialRxLineOpen_ = false;
}

void MainWindow::appendSerialRxBytes(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return;
    QTextCursor c(serialTerminal_->document());
    c.movePosition(QTextCursor::End);
    for (uint8_t b : bytes) {
        if (b == '\r') continue;
        if (!serialRxLineOpen_) {
            if (c.position() > 0) c.insertText("\n");
            c.insertText("< ");
            serialRxLineOpen_ = true;
        }
        if (b == '\n') {
            c.insertText("\n");
            serialRxLineOpen_ = false;
        } else if (b >= 0x20 && b < 0x7F) {
            c.insertText(QString(QChar(b)));
        } else {
            c.insertText(QString("."));  // non printable: keep the line readable
        }
    }
    serialTerminal_->setTextCursor(c);
    serialTerminal_->ensureCursorVisible();
}

void MainWindow::onSerialSend() {
    const QString text = serialInput_->text();
    const QByteArray utf8 = text.toUtf8();
    std::vector<uint8_t> bytes(utf8.begin(), utf8.end());
    if (serialCrLf_->isChecked()) {
        bytes.push_back('\r');
        bytes.push_back('\n');
    }
    if (bytes.empty()) return;
    // the bytes go to the board's virtual PC peer; the firmware receives them
    // only if it really routed PA10 to USART1_RX
    sim_.sendUsart1Bytes(bytes);
    appendSerialLine("> ", text);
    serialInput_->clear();
}

void MainWindow::onSerialClear() {
    // the Clear button only clears the view -- a board or simulator reset never
    // clears the terminal history
    serialTerminal_->clear();
    serialRxLineOpen_ = false;
}

// ---------------------------------------------------------------------------
// slots
// ---------------------------------------------------------------------------
void MainWindow::openFirmware() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open firmware"), QString(),
        tr("Firmware (*.hex *.bin);;Intel HEX (*.hex);;Binary (*.bin)"));
    if (path.isEmpty()) return;
    QMetaObject::invokeMethod(&sim_, "loadFirmware", Qt::QueuedConnection,
                              Q_ARG(QString, path));
}

void MainWindow::onRunClicked() { sim_.startRun(); }

void MainWindow::onPauseClicked() { sim_.pause(); }

void MainWindow::onResetClicked() {
    sim_.pause();
    QMetaObject::invokeMethod(&sim_, "slotReset", Qt::QueuedConnection);
}

void MainWindow::onStepClicked() {
    QMetaObject::invokeMethod(&sim_, "slotStep", Qt::QueuedConnection,
                              Q_ARG(int, 1));
}

void MainWindow::onStateChanged(const SimSnapshot& snap) {
    for (int i = 0; i < 8; i++) leds_[i]->setOn(snap.leds[i]);
    for (int i = 0; i < 4; i++)
        keyBtns_[i]->setText(QString("B%1%2").arg(i + 1).arg(
            (snap.buttons >> i) & 1 ? QString(" *") : QString()));

    pcVal_->setText(QString("0x%1").arg(snap.pc, 8, 16, QChar('0')));
    spVal_->setText(QString("0x%1").arg(snap.sp, 8, 16, QChar('0')));
    lrVal_->setText(QString("0x%1").arg(snap.lr, 8, 16, QChar('0')));
    xpsrVal_->setText(QString("0x%1").arg(snap.xpsr, 8, 16, QChar('0')));
    ipsrVal_->setText(QString("0x%1").arg(snap.ipsr, 8, 16, QChar('0')));
    for (int i = 0; i < 13; i++)
        regVals_[i]->setText(
            QString("0x%1").arg(snap.regs[i], 8, 16, QChar('0')));
    cyclesVal_->setText(QString::number(snap.cycles));
    // real-time factor: tells "very slow" apart from "hung" at a glance (the
    // [perf] lines in the log explain where the time goes when it is slow)
    clockVal_->setText(QString("%1 MHz   %2x")
                           .arg(snap.sysclkHz / 1000000.0, 0, 'f', 1)
                           .arg(snap.realTimeX, 0, 'f', 2));
    excVal_->setText(snap.activeException == 0
                         ? "thread"
                         : QString("IRQ %1 (%2 returns)")
                               .arg(snap.activeException)
                               .arg(snap.exceptionReturns));

    systickVal_->setText(QString("CSR=0x%1 RVR=%2 CVR=%3")
                             .arg(snap.systickCsr, 2, 16, QChar('0'))
                             .arg(snap.systickRvr)
                             .arg(snap.systickCvr));
    gpioCVal_->setText(QString("ODR=0x%1 IDR=0x%2")
                           .arg(snap.gpioC_Odr & 0xFFFF, 4, 16, QChar('0'))
                           .arg(snap.gpioC_Idr & 0xFFFF, 4, 16, QChar('0')));
    gpioABVal_->setText(QString("GPIOA.IDR=0x%1 GPIOB.IDR=0x%2")
                            .arg(snap.gpioA_Idr & 0xFFFF, 4, 16, QChar('0'))
                            .arg(snap.gpioB_Idr & 0xFFFF, 4, 16, QChar('0')));
    rccVal_->setText(QString("CR=0x%1 CFGR=0x%2")
                         .arg(snap.rccCr, 8, 16, QChar('0'))
                         .arg(snap.rccCfgr, 8, 16, QChar('0')));
    lcdVal_->setText(QString("cmds=%1 pixels=%2 last=0x%3")
                         .arg(snap.lcdCommandWrites)
                         .arg(snap.lcdPixelWrites)
                         .arg(snap.lcdLastCommand, 4, 16, QChar('0')));

    // signal / analog panel: pin-level measurements and source settings
    pa7StatusVal_->setText(snap.pa7Active ? "ACTIVE" : "INACTIVE");
    if (snap.pa7Active) {
        pa7FreqVal_->setText(QString("%1 Hz")
                                 .arg(snap.pa7Frequency, 0, 'f', 1));
        pa7DutyVal_->setText(QString("%1 %").arg(snap.pa7Duty * 100.0, 0, 'f',
                                                  1));
    } else {
        pa7FreqVal_->setText("0 Hz");
        pa7DutyVal_->setText("--");
    }
    pa7LevelVal_->setText(snap.pa7Level ? "high" : "low");
    pa7EdgesVal_->setText(QString::number(snap.pa7Edges));
    pulseStateVal_->setText(
        QString("%1 @ %2 Hz, pin %3")
            .arg(snap.pulseEnabled ? "on" : "off")
            .arg(snap.pulseFrequency, 0, 'f', 0)
            .arg(snap.pulseLevel ? "high" : "low"));
    pb4StateVal_->setText(
        QString("%1 @ %2 Hz, pin %3")
            .arg(snap.pb4PulseEnabled ? "on" : "off")
            .arg(snap.pb4PulseFrequency, 0, 'f', 0)
            .arg(snap.pb4PulseLevel ? "high" : "low"));
    r37Val_->setText(QString("%1 V").arg(snap.r37Voltage, 0, 'f', 2));
    r38Val_->setText(QString("%1 V").arg(snap.r38Voltage, 0, 'f', 2));

    // serial terminal: the firmware's own USART1 configuration + the bytes it
    // transmitted since the last snapshot
    if (snap.usart1Enabled)
        serialStatusVal_->setText(QString("enabled  (TE=%1 RE=%2)")
                                      .arg(snap.usart1TxEnabled ? 1 : 0)
                                      .arg(snap.usart1RxEnabled ? 1 : 0));
    else if (snap.usart1ClockEnabled)
        serialStatusVal_->setText("clocked, UE=0");
    else
        serialStatusVal_->setText("disabled (USART1EN=0)");
    serialBaudVal_->setText(snap.usart1Baud
                                ? QString("%1 baud").arg(snap.usart1Baud)
                                : QString("--"));
    serialFormatVal_->setText(
        QString("%1%2%3")
            .arg(snap.usart1WordBits)
            .arg(snap.usart1Parity == 0
                     ? "N"
                     : (snap.usart1Parity == 1 ? "E" : "O"))
            .arg(stopBitsText(snap.usart1StopBitsHalf)));
    serialFlagsVal_->setText(QString("RXNE=%1 TC=%2 ORE=%3")
                                 .arg(snap.usart1Rxne ? 1 : 0)
                                 .arg(snap.usart1Tc ? 1 : 0)
                                 .arg(snap.usart1Ore ? 1 : 0));
    serialCountersVal_->setText(QString("MCU->PC %1  PC->MCU %2  (queued %3)")
                                    .arg(snap.usart1TxTotal)
                                    .arg(snap.usart1RxTotal)
                                    .arg(snap.usart1Queued));
    if (snap.usart1TxPending > 0) appendSerialRxBytes(sim_.takeUsart1TxBytes());

    // LCD panel: pull the framebuffer only when the module changed a pixel
    // (dirty flag -- never copy 320*240*2 bytes on every snapshot).
    if (snap.lcdDirty) {
        sim_.copyLcdFramebuffer(lcdBuf_);
        lcd_->setFramebuffer(lcdBuf_);
        sim_.clearLcdDirty();
    }
}

void MainWindow::onRunningChanged(bool running) {
    runBtn_->setEnabled(!running);
    pauseBtn_->setEnabled(running);
    stepBtn_->setEnabled(!running);
    resetBtn_->setEnabled(true);
    statusBar()->showMessage(running ? tr("Running") : tr("Paused"), 0);
}

void MainWindow::onLog(const QString& msg) { logView_->appendPlainText(msg); }

void MainWindow::onFirmwareLoaded(const QString& summary) {
    fwLabel_->setText(summary.section('\n', 0, 0));
    runBtn_->setEnabled(true);
    statusBar()->showMessage(tr("Firmware loaded, reset done"), 3000);
}

void MainWindow::onFirmwareLoadFailed(const QString& error) {
    statusBar()->showMessage(tr("Load failed: %1").arg(error), 5000);
    logView_->appendPlainText("[error] " + error);
}

void MainWindow::onFaulted(const QString& error) {
    logView_->appendPlainText("[FAULT] " + error);
    statusBar()->showMessage(tr("FAULT: %1").arg(error));
}

// ---------------------------------------------------------------------------
// drag & drop
// ---------------------------------------------------------------------------
void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1) {
        const QString suffix = QFileInfo(urls.first().toLocalFile()).suffix()
                                   .toLower();
        if (suffix == "hex" || suffix == "bin") {
            event->acceptProposedAction();
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (urls.size() != 1) return;
    const QString path = urls.first().toLocalFile();
    QMetaObject::invokeMethod(&sim_, "loadFirmware", Qt::QueuedConnection,
                              Q_ARG(QString, path));
    event->acceptProposedAction();
}
