#pragma once

#include <QMainWindow>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sim/Simulator.h"

class QCheckBox;
class QDial;
class QDoubleSpinBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class LedIndicator;
class LcdWidget;
class DebugIpcServer;
class SimulatorDebugTarget;
class SimulatorFlashProgrammer;

// Debug IPC front-end configuration (stage 7-2A, driven by the command line).
// Default: no IPC server at all -- a plain `bluesim.exe` (or a double click)
// behaves exactly as it did before this stage (spec 10/139).
struct DebugIpcUiConfig {
    bool enabled = false;
    std::string pipeName;  // as given on the command line
    bool waitDebugger = false;       // --wait-debugger
    bool exitOnDisconnect = false;   // --exit-on-debugger-disconnect
    bool trace = false;              // --debug-ipc-trace
};

// ============================================================================
// Main window: virtual board (LCD panel, LED bank, buttons) + CPU /
// peripheral register views + run controls.
//
// All board interaction goes through the Simulator's hardware-accurate paths:
//   buttons -> board model -> GPIO IDR   (never direct firmware calls)
//   LED state <- board model <- GPIO ODR <- 74LS573 latch (PD2 edge)
//   LCD pixels <- board model <- LcdController GRAM <- GPIO bus writes
// ============================================================================
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const DebugIpcUiConfig& ipc = {},
                        QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void openFirmware();
    void onRunClicked();
    void onPauseClicked();
    void onResetClicked();
    void onStepClicked();

    void onStateChanged(const SimSnapshot& snap);
    void onRunningChanged(bool running);
    void onLog(const QString& msg);
    void onFirmwareLoaded(const QString& summary);
    void onFirmwareLoadFailed(const QString& error);
    void onFaulted(const QString& error);

    // serial terminal (stage 5)
    void onSerialSend();
    void onSerialClear();

    // stage 7-2A: owner-thread debug command pump + status display
    void onIpcTick();

private:
    QWidget* buildBoardPanel();
    QWidget* buildCpuPanel();
    QWidget* buildPeriphPanel();
    QWidget* buildSignalPanel();
    QWidget* buildSerialPanel();

    // terminal text helpers
    void appendSerialLine(const QString& prefix, const QString& text);
    void appendSerialRxBytes(const std::vector<uint8_t>& bytes);

    // stage 7-2A: create the Debug IPC server (only with --debug-pipe) and the
    // owner-thread pump timer. The GUI only shows status; it implements no
    // debugger UI.
    void setupDebugIpc(const DebugIpcUiConfig& cfg);

    Simulator sim_;

    // controls
    QPushButton* runBtn_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;
    QPushButton* resetBtn_ = nullptr;
    QPushButton* stepBtn_ = nullptr;
    // (no speed selector: the simulation always runs 1:1 with real time)
    QLabel* fwLabel_ = nullptr;

    // board widgets
    LedIndicator* leds_[8] = {};
    QPushButton* keyBtns_[4] = {};
    LcdWidget* lcd_ = nullptr;
    std::vector<uint16_t> lcdBuf_;  // GUI-side copy of the panel framebuffer

    // register view labels
    QLabel* pcVal_ = nullptr;
    QLabel* spVal_ = nullptr;
    QLabel* lrVal_ = nullptr;
    QLabel* xpsrVal_ = nullptr;
    QLabel* ipsrVal_ = nullptr;
    QLabel* regVals_[13] = {};
    QLabel* cyclesVal_ = nullptr;
    QLabel* clockVal_ = nullptr;
    QLabel* excVal_ = nullptr;

    QLabel* systickVal_ = nullptr;
    QLabel* gpioCVal_ = nullptr;
    QLabel* gpioABVal_ = nullptr;
    QLabel* rccVal_ = nullptr;
    QLabel* lcdVal_ = nullptr;

    // Signal / analog panel: PA7 waveform measured at the pin + PA15 pulse input
    // and the R37/R38 potentiometers. The GUI only sets board-level sources and
    // only reads the pin measurement -- it never touches a TIM/ADC register.
    QLabel* pa7StatusVal_ = nullptr;
    QLabel* pa7FreqVal_ = nullptr;
    QLabel* pa7DutyVal_ = nullptr;
    QLabel* pa7LevelVal_ = nullptr;
    QLabel* pa7EdgesVal_ = nullptr;
    QLabel* pulseStateVal_ = nullptr;
    QCheckBox* pulseEnable_ = nullptr;
    QDial* pulseDial_ = nullptr;
    QSpinBox* pulseSpin_ = nullptr;
    QLabel* pb4StateVal_ = nullptr;
    QCheckBox* pb4Enable_ = nullptr;
    QDial* pb4Dial_ = nullptr;
    QSpinBox* pb4Spin_ = nullptr;
    QLabel* r37Val_ = nullptr;
    QDial* r37Dial_ = nullptr;
    QDoubleSpinBox* r37Spin_ = nullptr;
    QLabel* r38Val_ = nullptr;
    QDial* r38Dial_ = nullptr;
    QDoubleSpinBox* r38Spin_ = nullptr;

    // Serial tab: the virtual PC terminal of the DAP-Link serial port. The GUI
    // only types bytes into the peer and displays what the firmware configured
    // and transmitted -- it never writes a USART register.
    QLabel* serialStatusVal_ = nullptr;
    QLabel* serialBaudVal_ = nullptr;
    QLabel* serialFormatVal_ = nullptr;
    QLabel* serialFlagsVal_ = nullptr;
    QLabel* serialCountersVal_ = nullptr;
    QPlainTextEdit* serialTerminal_ = nullptr;
    QLineEdit* serialInput_ = nullptr;
    QCheckBox* serialCrLf_ = nullptr;
    bool serialRxLineOpen_ = false;  // a "< " MCU line is still being written

    QPlainTextEdit* logView_ = nullptr;

    // ---- stage 7-2A debug IPC (only when --debug-pipe was given) -----------
    // The GUI never calls the target from the pump except through
    // DebugIpcServer::processPendingCommands(), which runs on THIS thread (the
    // simulator owner thread) -- the pipe worker only touches the queue.
    std::unique_ptr<SimulatorDebugTarget> debugTarget_;
    std::unique_ptr<SimulatorFlashProgrammer> flashProgrammer_;
    std::unique_ptr<DebugIpcServer> ipcServer_;
    QTimer* ipcTimer_ = nullptr;  // owner-thread pump + status refresh
    QLabel* ipcStatusVal_ = nullptr;
    QLabel* ipcTargetVal_ = nullptr;
    QLabel* ipcStopVal_ = nullptr;
    bool ipcWasConnected_ = false;
    bool ipcExitOnDisconnect_ = false;
    QString ipcStatusText_;
    QString ipcTargetText_;
    QString ipcStopText_;
};
