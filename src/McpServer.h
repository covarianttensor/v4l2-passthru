#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <deque>
#include <functional>
#include <thread>

class CaptureController;
class InspectionService;

// Stdio Model Context Protocol (MCP) server for autonomous on-glass HDMI inspection.
// Connects to a live GUI/headless v4l2-passthru instance over QLocalSocket when available,
// and falls back to an on-demand in-process CaptureController when no viewer is running.
class McpServer : public QObject
{
    Q_OBJECT

public:
    explicit McpServer(const QString &socketPath, QObject *parent = nullptr);
    ~McpServer() override;

    void start();

private slots:
    void onStdinLine(const QByteArray &line);
    void onStdinClosed();
    void onIdleReleaseTimeout();

private:
    void handleMessage(const QJsonObject &msg);
    void handleInitialize(const QJsonValue &id, const QJsonObject &params);
    void handleToolsList(const QJsonValue &id);
    void handleToolsCall(const QJsonValue &id, const QJsonObject &params);
    void processNextToolCall();

    void dispatchInspection(const QString &method,
                            const QJsonObject &params,
                            bool allowAutoStart,
                            std::function<void(const QJsonObject &, const QString &sourceMode)> callback);
    void dispatchViaSocket(const QString &method,
                           const QJsonObject &params,
                           std::function<void(bool connected, const QJsonObject &resp)> callback);
    void ensureFallbackStarted(bool waitForFirstFrame, std::function<void()> readyCallback);
    void releaseFallback();

    void sendResult(const QJsonValue &id, const QJsonObject &result);
    void sendError(const QJsonValue &id, int code, const QString &message);
    void writeJsonLine(const QJsonObject &obj);
    void finishInFlight();

    struct PendingToolCall {
        QJsonValue id;
        QJsonObject params;
    };

    QString m_socketPath;
    CaptureController *m_fallbackController = nullptr;
    InspectionService *m_fallbackService = nullptr;
    bool m_fallbackActive = false;
    QTimer m_idleReleaseTimer;

    std::deque<PendingToolCall> m_toolQueue;
    bool m_toolRunning = false;
    int m_inFlightCount = 0;
    bool m_stdinClosed = false;
    std::atomic<bool> m_stopReader{false};
    std::thread m_stdinThread;
};
