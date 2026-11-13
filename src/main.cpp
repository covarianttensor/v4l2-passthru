#include "CaptureController.h"

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

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("v4l2-passthru"));
    app.setApplicationName(QStringLiteral("v4l2-passthru"));
    app.setApplicationDisplayName(QStringLiteral("V4L2 Passthru"));
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
    parser.addOption(smokeOpt);
    parser.addOption(screenshotOpt);
    parser.process(app);

    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("V4L2Passthru"), QStringLiteral("Main"));

    if (parser.isSet(screenshotOpt)) {
        const QString outPath = parser.value(screenshotOpt);
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        auto *ctrl = engine.singletonInstance<CaptureController *>(QStringLiteral("V4L2Passthru"),
                                                                  QStringLiteral("CaptureController"));
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
        auto *ctrl = engine.singletonInstance<CaptureController *>(QStringLiteral("V4L2Passthru"),
                                                                  QStringLiteral("CaptureController"));
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
