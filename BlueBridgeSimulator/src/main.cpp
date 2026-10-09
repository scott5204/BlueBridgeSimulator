#include <QApplication>

#include <cstdio>

#include "gui/MainWindow.h"

// ============================================================================
// bluesim -- CT117E-M4 (STM32G431RBT6) simulator GUI.
//
// Stage 7-2A command line (all optional; without them the behaviour is exactly
// what it always was -- no debug IPC server, no extra thread):
//
//   --debug-pipe <name>            enable the Debug IPC named-pipe server on
//                                  <name> (with or without the \\.\pipe\
//                                  prefix). The name must be unique per
//                                  simulator instance; the future AGDI DLL
//                                  generates e.g.
//                                  \\.\pipe\BlueBridgeSimulator.Debug.<pid>.
//   --wait-debugger                keep the target Halted until a debugger
//                                  session connects (Run is suppressed).
//   --exit-on-debugger-disconnect  quit when the debugger disconnects
//                                  (default: keep running).
//   --debug-ipc-trace              log one line per IPC packet
//                                  (opcode/requestId/size/status).
// ============================================================================

namespace {

struct CliOptions {
    DebugIpcUiConfig ipc;
};

void printUsage() {
    std::printf(
        "usage: bluesim [--debug-pipe <name>] [--wait-debugger]\n"
        "               [--exit-on-debugger-disconnect] [--debug-ipc-trace]\n"
        "\n"
        "  --debug-pipe <name>            Debug IPC named pipe "
        "(e.g. BlueBridgeSimulator.Debug.1234.A1B2)\n"
        "  --wait-debugger                stay Halted until a debugger "
        "connects\n"
        "  --exit-on-debugger-disconnect  quit when the debugger disconnects\n"
        "  --debug-ipc-trace              log every IPC packet\n");
}

bool parseArgs(const QStringList& args, CliOptions& out, QString& error) {
    for (int i = 1; i < args.size(); i++) {
        const QString a = args.at(i);
        if (a == "--debug-pipe") {
            if (i + 1 >= args.size()) {
                error = "--debug-pipe requires a pipe name";
                return false;
            }
            out.ipc.pipeName = args.at(++i).toStdString();
            out.ipc.enabled = true;
        } else if (a == "--wait-debugger") {
            out.ipc.waitDebugger = true;
        } else if (a == "--exit-on-debugger-disconnect") {
            out.ipc.exitOnDisconnect = true;
        } else if (a == "--debug-ipc-trace") {
            out.ipc.trace = true;
        } else if (a == "--help" || a == "-h") {
            printUsage();
            std::exit(0);
        } else {
            error = QString("unknown argument '%1'").arg(a);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("BlueBridgeSimulator");
    app.setOrganizationName("BlueBridge");

    CliOptions cli;
    QString error;
    if (!parseArgs(app.arguments(), cli, error)) {
        std::fprintf(stderr, "bluesim: %s\n", error.toLocal8Bit().constData());
        printUsage();
        return 2;
    }
    if (cli.ipc.waitDebugger && !cli.ipc.enabled) {
        std::fprintf(stderr,
                     "bluesim: --wait-debugger has no effect without "
                     "--debug-pipe\n");
    }

    MainWindow w(cli.ipc);
    w.show();
    return app.exec();
}