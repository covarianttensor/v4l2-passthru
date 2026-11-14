#include "CaptureController.h"
#include "InspectionEngine.h"
#include "IpcServer.h"
#include "McpServer.h"

#include <gst/gst.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

bool hasArg(int argc, char *argv[], const char *flag)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

void stderrMessageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *level = "INFO";
    switch (type) {
    case QtDebugMsg: level = "DEBUG"; break;
    case QtInfoMsg: level = "INFO"; break;
    case QtWarningMsg: level = "WARN"; break;
    case QtCriticalMsg: level = "CRIT"; break;
    case QtFatalMsg: level = "FATAL"; break;
    }
    const QByteArray utf8 = msg.toUtf8();
    std::fprintf(stderr, "[%s] %s\n", level, utf8.constData());
    std::fflush(stderr);
    if (type == QtFatalMsg) {
        std::abort();
    }
}

} // namespace

int main(int argc, char *argv[])
{
    const bool mcpMode = hasArg(argc, argv, "--mcp");
    const bool headlessMode = hasArg(argc, argv, "--headless");
    const bool helpMode = hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h");

    if (mcpMode) {
        // Keep stdout strictly reserved for MCP JSON-RPC frames.
        qInstallMessageHandler(stderrMessageHandler);
    }
    if ((mcpMode || headlessMode || helpMode) && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    gst_init(&argc, &argv);

    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("v4l2-passthru"));
    app.setApplicationName(QStringLiteral("v4l2-passthru"));
    app.setApplicationDisplayName(QStringLiteral("V4L2 Passthru"));
    app.setApplicationVersion(QStringLiteral(V4L2_PASSTHRU_VERSION));
    app.setDesktopFileName(QStringLiteral("v4l2-passthru"));
    app.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/V4L2Passthru/assets/v4l2-passthru.svg")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Low-latency V4L2 video and audio passthrough viewer"));
    parser.addHelpOption();
    QCommandLineOption smokeOpt(
        QStringList{QStringLiteral("smoke-test")},
        QStringLiteral("Run for <seconds>, print stream telemetry, and exit."),
        QStringLiteral("seconds"));
    QCommandLineOption screenshotOpt(
        QStringList{QStringLiteral("screenshot")},
        QStringLiteral("Capture a live window screenshot to <path> and exit."),
        QStringLiteral("path"));
    QCommandLineOption mcpOpt(
        QStringList{QStringLiteral("mcp")},
        QStringLiteral("Run as a stdio MCP server for autonomous on-glass HDMI inspection."));
    QCommandLineOption headlessOpt(
        QStringList{QStringLiteral("headless")},
        QStringLiteral("Run capture + inspection IPC server without opening a GUI window."));
    QCommandLineOption socketOpt(
        QStringList{QStringLiteral("socket")},
        QStringLiteral("Override Unix socket path for live inspection IPC."),
        QStringLiteral("path"));
    parser.addOption(smokeOpt);
    parser.addOption(screenshotOpt);
    parser.addOption(mcpOpt);
    parser.addOption(headlessOpt);
    parser.addOption(socketOpt);
    parser.process(app);

    const QString socketPath = parser.isSet(socketOpt)
        ? parser.value(socketOpt)
        : InspectionEngine::defaultSocketPath();

    if (parser.isSet(mcpOpt)) {
        McpServer mcp(socketPath);
        mcp.start();
        return app.exec();
    }

    if (parser.isSet(headlessOpt)) {
        auto *ctrl = new CaptureController(&app);
        ctrl->setPersistSettings(false);
        ctrl->setSelectedVideoDevice(QString());
        ctrl->setAudioEnabled(false);
        auto *ipc = new IpcServer(ctrl, socketPath, &app);
        ipc->start();
        ctrl->start();
        return app.exec();
    }

    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("V4L2Passthru"), QStringLiteral("Main"));

    auto *ctrl = engine.singletonInstance<CaptureController *>(QStringLiteral("V4L2Passthru"),
                                                              QStringLiteral("CaptureController"));

    std::unique_ptr<IpcServer> ipcServer;
    if (ctrl && !parser.isSet(screenshotOpt) && !parser.isSet(smokeOpt)) {
        ipcServer = std::make_unique<IpcServer>(ctrl, socketPath);
        ipcServer->start();
    }

    if (parser.isSet(screenshotOpt)) {
        const QString outPath = parser.value(screenshotOpt);
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QTimer::singleShot(2000, &app, [win, ctrl, outPath, &app]() {
            if (!win || !ctrl || !ctrl->streaming()) {
                qCritical() << "Screenshot failed: window or live stream not ready";
                app.exit(1);
                return;
            }
            const QImage img = win->grabWindow();
            const bool ok = !img.isNull() && img.save(outPath);
            qInfo() << "Saved screenshot:" << outPath << "ok=" << ok << "size=" << img.size();
            app.exit(ok ? 0 : 1);
        });
    } else if (parser.isSet(smokeOpt)) {
        const int secs = qMax(1, parser.value(smokeOpt).toInt());
        QTimer::singleShot(secs * 1000, &app, [ctrl, &app]() {
            if (!ctrl) {
                qCritical() << "Smoke test failed: CaptureController singleton unavailable";
                app.exit(2);
                return;
            }
            qInfo() << "Smoke test result:"
                    << "streaming=" << ctrl->streaming()
                    << "status=" << ctrl->statusText()
                    << "resolution=" << ctrl->activeResolution()
                    << "fps=" << ctrl->measuredFps()
                    << "format=" << ctrl->pixelFormat()
                    << "reconnects=" << ctrl->reconnectCount();
            app.exit(ctrl->streaming() ? 0 : 1);
        });
    }

    return app.exec();
}
