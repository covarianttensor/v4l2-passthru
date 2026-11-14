#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QString>

#include <functional>

class CaptureController;

// Executes on-glass inspection commands asynchronously against a CaptureController.
class InspectionService : public QObject
{
    Q_OBJECT

public:
    explicit InspectionService(CaptureController *controller, QObject *parent = nullptr);

    using Callback = std::function<void(const QJsonObject &)>;
    void execute(const QJsonObject &request, Callback callback);

private:
    void handleStatus(const QJsonObject &params, Callback callback);
    void handleGrabFrame(const QJsonObject &params, Callback callback);
    void handleOcrScreen(const QJsonObject &params, Callback callback);
    void handleWaitForChange(const QJsonObject &params, Callback callback);
    void handleWaitForSignal(const QJsonObject &params, Callback callback);
    void handleCaptureBurst(const QJsonObject &params, Callback callback);
    void handleControl(const QJsonObject &params, Callback callback);

    CaptureController *m_controller = nullptr;
};

// Local Unix socket server exposing InspectionService to external tools (e.g., --mcp).
class IpcServer : public QObject
{
    Q_OBJECT

public:
    explicit IpcServer(CaptureController *controller,
                       const QString &socketPath,
                       QObject *parent = nullptr);
    ~IpcServer() override;

    bool start();
    QString socketPath() const;

    // Framing helpers shared with IpcClient in McpServer.
    static bool writeFramedJson(QLocalSocket *socket, const QJsonObject &obj, int timeoutMs = 5000);
    static bool readFramedJsonBlocking(QLocalSocket *socket, QJsonObject *out, int timeoutMs = 30000);

private slots:
    void onNewConnection();

private:
    void handleClientData(QLocalSocket *client, QByteArray *buffer);

    CaptureController *m_controller = nullptr;
    InspectionService *m_service = nullptr;
    QLocalServer *m_server = nullptr;
    QString m_socketPath;
};
