#include "McpServer.h"

#include "CaptureController.h"
#include "DeviceDiscovery.h"
#include "InspectionEngine.h"
#include "IpcServer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMetaObject>
#include <QPointer>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {

constexpr int kFallbackIdleReleaseMs = 90000;
constexpr int kFirstFrameWaitMs = 3500;

QJsonObject propSchema(const QString &type, const QString &description, const QJsonValue &defaultVal = QJsonValue())
{
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), type);
    obj.insert(QStringLiteral("description"), description);
    if (!defaultVal.isUndefined() && !defaultVal.isNull()) {
        obj.insert(QStringLiteral("default"), defaultVal);
    }
    return obj;
}

QJsonObject cropPropertySchema()
{
    QJsonObject props;
    props.insert(QStringLiteral("x"), propSchema(QStringLiteral("number"), QStringLiteral("Left offset (0..1 normalized or pixels)")));
    props.insert(QStringLiteral("y"), propSchema(QStringLiteral("number"), QStringLiteral("Top offset (0..1 normalized or pixels)")));
    props.insert(QStringLiteral("width"), propSchema(QStringLiteral("number"), QStringLiteral("Width (0..1 normalized or pixels)")));
    props.insert(QStringLiteral("height"), propSchema(QStringLiteral("number"), QStringLiteral("Height (0..1 normalized or pixels)")));
    props.insert(QStringLiteral("units"), propSchema(QStringLiteral("string"), QStringLiteral("Optional 'norm' or 'px'")));

    QJsonObject crop;
    crop.insert(QStringLiteral("type"), QStringLiteral("object"));
    crop.insert(QStringLiteral("description"), QStringLiteral("Optional crop region"));
    crop.insert(QStringLiteral("properties"), props);
    return crop;
}

QJsonObject makeToolDef(const QString &name,
                        const QString &description,
                        const QJsonObject &properties,
                        const QStringList &required = {})
{
    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    if (!required.isEmpty()) {
        QJsonArray req;
        for (const QString &r : required) {
            req.append(r);
        }
        schema.insert(QStringLiteral("required"), req);
    }

    QJsonObject tool;
    tool.insert(QStringLiteral("name"), name);
    tool.insert(QStringLiteral("description"), description);
    tool.insert(QStringLiteral("inputSchema"), schema);
    return tool;
}

QJsonArray buildToolDefinitions()
{
    QJsonArray tools;

    {
        QJsonObject props;
        props.insert(QStringLiteral("autoStart"),
                     propSchema(QStringLiteral("boolean"),
                                QStringLiteral("Start headless capture if no live viewer is running"),
                                false));
        tools.append(makeToolDef(
            QStringLiteral("get_status"),
            QStringLiteral("Get HDMI capture pipeline health, resolution, measured FPS, reconnect count, frame age, and V4L2 device list."),
            props));
    }

    {
        QJsonObject props;
        props.insert(QStringLiteral("format"),
                     propSchema(QStringLiteral("string"), QStringLiteral("Image format: 'png' (crisp text) or 'jpeg'"), QStringLiteral("png")));
        props.insert(QStringLiteral("quality"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("JPEG quality 10..100"), 90));
        props.insert(QStringLiteral("maxWidth"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Optional max output width in pixels (0 = native resolution)"), 0));
        props.insert(QStringLiteral("crop"), cropPropertySchema());
        props.insert(QStringLiteral("ocr"),
                     propSchema(QStringLiteral("boolean"), QStringLiteral("Run Tesseract OCR on the captured frame"), false));
        props.insert(QStringLiteral("invertOcr"),
                     propSchema(QStringLiteral("boolean"), QStringLiteral("Invert image colors before OCR"), false));
        props.insert(QStringLiteral("savePath"),
                     propSchema(QStringLiteral("string"), QStringLiteral("Optional absolute file path to save the captured frame")));
        tools.append(makeToolDef(
            QStringLiteral("grab_frame"),
            QStringLiteral("Capture the current raw HDMI video frame (without HUD overlay) for visual on-glass inspection."),
            props));
    }

    {
        QJsonObject props;
        props.insert(QStringLiteral("crop"), cropPropertySchema());
        props.insert(QStringLiteral("invert"),
                     propSchema(QStringLiteral("boolean"), QStringLiteral("Invert colors before OCR (helpful for some terminal/BIOS themes)"), false));
        props.insert(QStringLiteral("psm"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Tesseract page segmentation mode (0..13, default 6)"), 6));
        tools.append(makeToolDef(
            QStringLiteral("ocr_screen"),
            QStringLiteral("Run local Tesseract OCR on the current HDMI frame and return extracted screen text."),
            props));
    }

    {
        QJsonObject props;
        props.insert(QStringLiteral("timeoutMs"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Max milliseconds to wait for visual change (250..60000)"), 15000));
        props.insert(QStringLiteral("pollIntervalMs"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Polling interval in milliseconds (50..2000)"), 200));
        props.insert(QStringLiteral("changeThreshold"),
                     propSchema(QStringLiteral("number"), QStringLiteral("Fraction of pixels [0.001..1.0] that must change"), 0.015));
        props.insert(QStringLiteral("settleMs"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Delay after change before snapping final frame (0..5000)"), 400));
        props.insert(QStringLiteral("format"),
                     propSchema(QStringLiteral("string"), QStringLiteral("'png' or 'jpeg'"), QStringLiteral("png")));
        props.insert(QStringLiteral("maxWidth"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Optional max output width (0 = native)"), 0));
        props.insert(QStringLiteral("crop"), cropPropertySchema());
        props.insert(QStringLiteral("ocr"),
                     propSchema(QStringLiteral("boolean"), QStringLiteral("Run OCR on the resulting frame"), false));
        props.insert(QStringLiteral("invertOcr"),
                     propSchema(QStringLiteral("boolean"), QStringLiteral("Invert colors before OCR"), false));
        props.insert(QStringLiteral("savePath"),
                     propSchema(QStringLiteral("string"), QStringLiteral("Optional file path to save the resulting frame")));
        tools.append(makeToolDef(
            QStringLiteral("wait_for_change"),
            QStringLiteral("Wait until the HDMI screen visually changes (e.g., reboot -> BIOS/rEFInd/GRUB) and return the settled frame."),
            props));
    }

    {
        QJsonObject props;
        props.insert(QStringLiteral("state"),
                     propSchema(QStringLiteral("string"), QStringLiteral("Target signal state: 'streaming' or 'lost'"), QStringLiteral("streaming")));
        props.insert(QStringLiteral("timeoutMs"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Max wait time in milliseconds (250..60000)"), 15000));
        tools.append(makeToolDef(
            QStringLiteral("wait_for_signal"),
            QStringLiteral("Wait until the HDMI capture stream becomes active ('streaming') or drops ('lost')."),
            props));
    }

    {
        QJsonObject props;
        props.insert(QStringLiteral("durationMs"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Total burst window in ms (200..30000)"), 8000));
        props.insert(QStringLiteral("intervalMs"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Sampling interval in ms (100..5000)"), 350));
        props.insert(QStringLiteral("onlyChanged"),
                     propSchema(QStringLiteral("boolean"), QStringLiteral("Only keep frames that differ from the previous kept frame"), true));
        props.insert(QStringLiteral("changeThreshold"),
                     propSchema(QStringLiteral("number"), QStringLiteral("Visual change threshold [0.001..1.0]"), 0.01));
        props.insert(QStringLiteral("maxFrames"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Max frames to return (1..20)"), 10));
        props.insert(QStringLiteral("maxWidth"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("Max output width per burst frame"), 1280));
        props.insert(QStringLiteral("format"),
                     propSchema(QStringLiteral("string"), QStringLiteral("'jpeg' or 'png'"), QStringLiteral("jpeg")));
        props.insert(QStringLiteral("quality"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("JPEG quality 10..100"), 85));
        props.insert(QStringLiteral("ocr"),
                     propSchema(QStringLiteral("boolean"), QStringLiteral("Run OCR on each captured burst frame"), false));
        props.insert(QStringLiteral("invertOcr"),
                     propSchema(QStringLiteral("boolean"), QStringLiteral("Invert colors before OCR"), false));
        props.insert(QStringLiteral("outputDir"),
                     propSchema(QStringLiteral("string"), QStringLiteral("Optional directory to save burst frames and manifest.json")));
        tools.append(makeToolDef(
            QStringLiteral("capture_burst"),
            QStringLiteral("Capture a timeline of distinct frames over a reboot or boot-menu transition so transient errors are not missed."),
            props));
    }

    {
        QJsonObject props;
        props.insert(QStringLiteral("action"),
                     propSchema(QStringLiteral("string"),
                                QStringLiteral("Action: 'reconnect', 'refresh_devices', 'set_device', 'set_format', 'set_fps', 'set_resolution', or 'release'")));
        props.insert(QStringLiteral("videoDevice"),
                     propSchema(QStringLiteral("string"), QStringLiteral("For set_device: V4L2 device path (e.g. '/dev/video0') or '' for Auto (VC-003)")));
        props.insert(QStringLiteral("pixelFormat"),
                     propSchema(QStringLiteral("string"), QStringLiteral("For set_format: 'YUYV' or 'MJPG'")));
        props.insert(QStringLiteral("fps"),
                     propSchema(QStringLiteral("integer"), QStringLiteral("For set_fps: 30, 50, or 60")));
        props.insert(QStringLiteral("resolutionMode"),
                     propSchema(QStringLiteral("string"), QStringLiteral("For set_resolution: '1080p', '720p', or 'auto'")));
        tools.append(makeToolDef(
            QStringLiteral("control_capture"),
            QStringLiteral("Control the capture pipeline (reconnect, switch V4L2 device, YUYV/MJPG, fps/resolutionMode, or release fallback device)."),
            props,
            {QStringLiteral("action")}));
    }

    return tools;
}

QJsonObject buildMcpToolResponse(const QJsonObject &resp, const QString &toolName, const QString &sourceMode)
{
    const bool ok = resp.value(QStringLiteral("ok")).toBool(false);
    if (!ok) {
        QJsonObject errText;
        errText.insert(QStringLiteral("type"), QStringLiteral("text"));
        errText.insert(QStringLiteral("text"), resp.value(QStringLiteral("error")).toString(QStringLiteral("Inspection failed")));

        QJsonObject result;
        result.insert(QStringLiteral("isError"), true);
        result.insert(QStringLiteral("content"), QJsonArray{errText});
        return result;
    }

    QJsonArray content;
    QJsonObject meta = resp;
    meta.remove(QStringLiteral("ok"));
    meta.insert(QStringLiteral("sourceMode"), sourceMode);

    if (toolName == QStringLiteral("capture_burst")) {
        const QJsonArray frames = meta.value(QStringLiteral("frames")).toArray();
        QJsonArray slimFrames;
        for (const QJsonValue &val : frames) {
            QJsonObject f = val.toObject();
            const QString b64 = f.take(QStringLiteral("imageBase64")).toString();
            const QString mime = f.value(QStringLiteral("mimeType")).toString();
            slimFrames.append(f);

            if (!b64.isEmpty() && !mime.isEmpty()) {
                QJsonObject imgPart;
                imgPart.insert(QStringLiteral("type"), QStringLiteral("image"));
                imgPart.insert(QStringLiteral("data"), b64);
                imgPart.insert(QStringLiteral("mimeType"), mime);
                content.append(imgPart);
            }
        }
        meta.insert(QStringLiteral("frames"), slimFrames);
    } else if (meta.contains(QStringLiteral("imageBase64"))) {
        const QString b64 = meta.take(QStringLiteral("imageBase64")).toString();
        const QString mime = meta.value(QStringLiteral("mimeType")).toString();
        if (!b64.isEmpty() && !mime.isEmpty()) {
            QJsonObject imgPart;
            imgPart.insert(QStringLiteral("type"), QStringLiteral("image"));
            imgPart.insert(QStringLiteral("data"), b64);
            imgPart.insert(QStringLiteral("mimeType"), mime);
            content.append(imgPart);
        }
    }

    QJsonObject textPart;
    textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
    textPart.insert(QStringLiteral("text"), QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Indented)));
    content.prepend(textPart);

    QJsonObject result;
    result.insert(QStringLiteral("content"), content);
    return result;
}

} // namespace

McpServer::McpServer(const QString &socketPath, QObject *parent)
    : QObject(parent)
    , m_socketPath(socketPath)
{
    m_idleReleaseTimer.setSingleShot(true);
    m_idleReleaseTimer.setInterval(kFallbackIdleReleaseMs);
    connect(&m_idleReleaseTimer, &QTimer::timeout, this, &McpServer::onIdleReleaseTimeout);
}

McpServer::~McpServer()
{
    m_stopReader.store(true);
    releaseFallback();
    if (m_stdinThread.joinable()) {
        // Detach because std::getline on stdin may block until EOF from MCP client.
        m_stdinThread.detach();
    }
}

void McpServer::start()
{
    QPointer<McpServer> self(this);
    m_stdinThread = std::thread([self]() {
        std::string line;
        while (!self.isNull() && std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            const QByteArray payload(line.data(), static_cast<qsizetype>(line.size()));
            QMetaObject::invokeMethod(
                self.data(),
                [self, payload]() {
                    if (self) {
                        self->onStdinLine(payload);
                    }
                },
                Qt::QueuedConnection);
        }

        if (self) {
            QMetaObject::invokeMethod(
                self.data(),
                [self]() {
                    if (self) {
                        self->onStdinClosed();
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

void McpServer::onStdinLine(const QByteArray &line)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        sendError(QJsonValue::Null, -32700, QStringLiteral("Parse error"));
        return;
    }

    handleMessage(doc.object());
}

void McpServer::onStdinClosed()
{
    m_stdinClosed = true;
    if (m_inFlightCount <= 0) {
        releaseFallback();
        QCoreApplication::quit();
    }
}

void McpServer::finishInFlight()
{
    if (m_inFlightCount > 0) {
        --m_inFlightCount;
    }
    if (m_stdinClosed && m_inFlightCount <= 0) {
        releaseFallback();
        QCoreApplication::quit();
    }
}

void McpServer::onIdleReleaseTimeout()
{
    if (m_fallbackActive) {
        qInfo() << "Releasing idle headless fallback capture device";
        releaseFallback();
    }
}

void McpServer::handleMessage(const QJsonObject &msg)
{
    const QString method = msg.value(QStringLiteral("method")).toString();
    const bool hasId = msg.contains(QStringLiteral("id"));
    const QJsonValue id = msg.value(QStringLiteral("id"));
    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();

    // JSON-RPC notifications have no "id" and must not receive a response.
    if (!hasId) {
        return;
    }

    if (method == QStringLiteral("initialize")) {
        handleInitialize(id, params);
    } else if (method == QStringLiteral("ping")) {
        sendResult(id, QJsonObject());
    } else if (method == QStringLiteral("tools/list")) {
        handleToolsList(id);
    } else if (method == QStringLiteral("tools/call")) {
        handleToolsCall(id, params);
    } else {
        sendError(id, -32601, QStringLiteral("Method not found: %1").arg(method));
    }
}

void McpServer::handleInitialize(const QJsonValue &id, const QJsonObject &params)
{
    QString protocolVersion = params.value(QStringLiteral("protocolVersion")).toString();
    if (protocolVersion.isEmpty()) {
        protocolVersion = QStringLiteral("2024-11-05");
    }

    QJsonObject toolsCap;
    toolsCap.insert(QStringLiteral("listChanged"), false);

    QJsonObject caps;
    caps.insert(QStringLiteral("tools"), toolsCap);

    QJsonObject serverInfo;
    serverInfo.insert(QStringLiteral("name"), QStringLiteral("v4l2-passthru-mcp"));
    serverInfo.insert(QStringLiteral("version"), QCoreApplication::applicationVersion().isEmpty()
                                                ? QStringLiteral("0.2.0")
                                                : QCoreApplication::applicationVersion());

    QJsonObject result;
    result.insert(QStringLiteral("protocolVersion"), protocolVersion);
    result.insert(QStringLiteral("capabilities"), caps);
    result.insert(QStringLiteral("serverInfo"), serverInfo);

    sendResult(id, result);
}

void McpServer::handleToolsList(const QJsonValue &id)
{
    QJsonObject result;
    result.insert(QStringLiteral("tools"), buildToolDefinitions());
    sendResult(id, result);
}

void McpServer::handleToolsCall(const QJsonValue &id, const QJsonObject &params)
{
    m_toolQueue.push_back(PendingToolCall{id, params});
    ++m_inFlightCount;
    if (!m_toolRunning) {
        processNextToolCall();
    }
}

void McpServer::processNextToolCall()
{
    if (m_toolRunning || m_toolQueue.empty()) {
        return;
    }

    const PendingToolCall next = m_toolQueue.front();
    m_toolQueue.pop_front();
    m_toolRunning = true;

    const QJsonValue id = next.id;
    const QString name = next.params.value(QStringLiteral("name")).toString();
    const QJsonObject args = next.params.value(QStringLiteral("arguments")).toObject();

    QString ipcMethod;
    bool allowAutoStart = true;

    if (name == QStringLiteral("get_status")) {
        ipcMethod = QStringLiteral("status");
        allowAutoStart = args.value(QStringLiteral("autoStart")).toBool(false);
    } else if (name == QStringLiteral("grab_frame")) {
        ipcMethod = QStringLiteral("grab_frame");
    } else if (name == QStringLiteral("ocr_screen")) {
        ipcMethod = QStringLiteral("ocr_screen");
    } else if (name == QStringLiteral("wait_for_change")) {
        ipcMethod = QStringLiteral("wait_for_change");
    } else if (name == QStringLiteral("wait_for_signal")) {
        ipcMethod = QStringLiteral("wait_for_signal");
    } else if (name == QStringLiteral("capture_burst")) {
        ipcMethod = QStringLiteral("capture_burst");
    } else if (name == QStringLiteral("control_capture")) {
        ipcMethod = QStringLiteral("control");
        if (args.value(QStringLiteral("action")).toString() == QStringLiteral("release")) {
            releaseFallback();
            QJsonObject resp;
            resp.insert(QStringLiteral("ok"), true);
            resp.insert(QStringLiteral("action"), QStringLiteral("release"));
            resp.insert(QStringLiteral("released"), true);
            sendResult(id, buildMcpToolResponse(resp, name, QStringLiteral("standby")));
            m_toolRunning = false;
            finishInFlight();
            processNextToolCall();
            return;
        }
    } else {
        sendError(id, -32602, QStringLiteral("Unknown tool: %1").arg(name));
        m_toolRunning = false;
        finishInFlight();
        processNextToolCall();
        return;
    }

    QPointer<McpServer> self(this);
    dispatchInspection(ipcMethod, args, allowAutoStart, [self, id, name](const QJsonObject &resp, const QString &mode) {
        if (!self) {
            return;
        }
        self->sendResult(id, buildMcpToolResponse(resp, name, mode));
        self->m_toolRunning = false;
        self->finishInFlight();
        self->processNextToolCall();
    });
}

void McpServer::dispatchInspection(const QString &method,
                                   const QJsonObject &params,
                                   bool allowAutoStart,
                                   std::function<void(const QJsonObject &, const QString &)> callback)
{
    QPointer<McpServer> self(this);
    dispatchViaSocket(method, params, [self, method, params, allowAutoStart, callback](bool connected, const QJsonObject &resp) {
        if (!self) {
            return;
        }
        if (connected) {
            // If a live GUI/headless instance appeared, release any fallback capture we held.
            if (self->m_fallbackActive) {
                self->releaseFallback();
            }
            callback(resp, QStringLiteral("live_ipc"));
            return;
        }

        if (method == QStringLiteral("status") && !allowAutoStart && !self->m_fallbackActive) {
            const auto devs = DeviceDiscovery::enumerateVideoDevices();
            QJsonArray devArr;
            for (const auto &dev : devs) {
                QJsonObject d;
                d.insert(QStringLiteral("devicePath"), dev.devicePath);
                d.insert(QStringLiteral("displayName"), dev.displayName);
                d.insert(QStringLiteral("vendorId"), dev.vendorId);
                d.insert(QStringLiteral("productId"), dev.productId);
                d.insert(QStringLiteral("isPreferred"), dev.isPreferred);
                devArr.append(d);
            }
            QJsonObject standby;
            standby.insert(QStringLiteral("ok"), true);
            standby.insert(QStringLiteral("streaming"), false);
            standby.insert(QStringLiteral("statusText"), QStringLiteral("Standby (no live v4l2-passthru GUI running)"));
            standby.insert(QStringLiteral("tesseractAvailable"), InspectionEngine::isTesseractAvailable());
            standby.insert(QStringLiteral("videoDevices"), devArr);
            callback(standby, QStringLiteral("standby"));
            return;
        }

        const bool waitForFirstFrame = (method == QStringLiteral("grab_frame")
                                        || method == QStringLiteral("ocr_screen"));
        self->ensureFallbackStarted(waitForFirstFrame, [self, method, params, callback]() {
            if (!self || !self->m_fallbackService) {
                return;
            }
            self->m_idleReleaseTimer.start();

            QJsonObject req;
            req.insert(QStringLiteral("method"), method);
            req.insert(QStringLiteral("params"), params);

            self->m_fallbackService->execute(req, [self, callback](const QJsonObject &fallbackResp) {
                if (self) {
                    self->m_idleReleaseTimer.start();
                }
                callback(fallbackResp, QStringLiteral("headless_fallback"));
            });
        });
    });
}

void McpServer::dispatchViaSocket(const QString &method,
                                  const QJsonObject &params,
                                  std::function<void(bool, const QJsonObject &)> callback)
{
    auto *socket = new QLocalSocket(this);
    auto buffer = std::make_shared<QByteArray>();
    auto done = std::make_shared<bool>(false);

    auto finish = [socket, done, callback](bool connected, const QJsonObject &resp) {
        if (*done) {
            return;
        }
        *done = true;
        socket->disconnect();
        socket->deleteLater();
        callback(connected, resp);
    };

    auto *timeoutTimer = new QTimer(socket);
    timeoutTimer->setSingleShot(true);
    timeoutTimer->setInterval(65000);
    connect(timeoutTimer, &QTimer::timeout, socket, [finish]() {
        QJsonObject err;
        err.insert(QStringLiteral("ok"), false);
        err.insert(QStringLiteral("error"), QStringLiteral("Timed out waiting for live IPC response"));
        finish(true, err);
    });

    connect(socket, &QLocalSocket::connected, socket, [socket, timeoutTimer, method, params]() {
        QJsonObject req;
        req.insert(QStringLiteral("method"), method);
        req.insert(QStringLiteral("params"), params);
        IpcServer::writeFramedJson(socket, req, 0);
        timeoutTimer->start();
    });

    connect(socket, &QLocalSocket::readyRead, socket, [socket, buffer, finish]() {
        buffer->append(socket->readAll());
        if (buffer->size() < 4) {
            return;
        }
        const quint32 len = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer->constData()));
        if (len == 0 || len > 64u * 1024u * 1024u) {
            QJsonObject err;
            err.insert(QStringLiteral("ok"), false);
            err.insert(QStringLiteral("error"), QStringLiteral("Invalid IPC frame length"));
            finish(true, err);
            return;
        }
        if (buffer->size() < 4 + static_cast<qsizetype>(len)) {
            return;
        }

        const QByteArray payload = buffer->mid(4, static_cast<qsizetype>(len));
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!doc.isObject()) {
            QJsonObject err;
            err.insert(QStringLiteral("ok"), false);
            err.insert(QStringLiteral("error"), QStringLiteral("Invalid IPC JSON response"));
            finish(true, err);
            return;
        }
        finish(true, doc.object());
    });

    connect(socket, &QLocalSocket::errorOccurred, socket, [finish](QLocalSocket::LocalSocketError) {
        finish(false, QJsonObject());
    });

    socket->connectToServer(m_socketPath);
}

void McpServer::ensureFallbackStarted(bool waitForFirstFrame, std::function<void()> readyCallback)
{
    if (!m_fallbackController) {
        m_fallbackController = new CaptureController(this);
        m_fallbackController->setPersistSettings(false);
        m_fallbackController->setSelectedVideoDevice(QString());
        // Disable audio in headless MCP fallback so we never interfere with PipeWire audio routing.
        m_fallbackController->setAudioEnabled(false);
        m_fallbackService = new InspectionService(m_fallbackController, this);
    }

    const bool justStarted = !m_fallbackActive;
    if (!m_fallbackActive) {
        m_fallbackActive = true;
        m_fallbackController->start();
    }
    m_idleReleaseTimer.start();

    if (!waitForFirstFrame || !justStarted || m_fallbackController->latestFrameSnapshot().frame.isValid()) {
        readyCallback();
        return;
    }

    const qint64 startMs = CaptureController::steadyClockNowMs();
    auto *timer = new QTimer(this);
    timer->setInterval(80);
    connect(timer, &QTimer::timeout, this, [this, timer, startMs, readyCallback]() {
        const bool hasFrame = m_fallbackController && m_fallbackController->latestFrameSnapshot().frame.isValid();
        const bool timedOut = (CaptureController::steadyClockNowMs() - startMs) >= kFirstFrameWaitMs;
        if (hasFrame || timedOut) {
            timer->stop();
            timer->deleteLater();
            readyCallback();
        }
    });
    timer->start();
}

void McpServer::releaseFallback()
{
    m_idleReleaseTimer.stop();
    if (m_fallbackController && m_fallbackActive) {
        m_fallbackController->stop();
        m_fallbackActive = false;
    }
}

void McpServer::sendResult(const QJsonValue &id, const QJsonObject &result)
{
    QJsonObject msg;
    msg.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    msg.insert(QStringLiteral("id"), id);
    msg.insert(QStringLiteral("result"), result);
    writeJsonLine(msg);
}

void McpServer::sendError(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject err;
    err.insert(QStringLiteral("code"), code);
    err.insert(QStringLiteral("message"), message);

    QJsonObject msg;
    msg.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    msg.insert(QStringLiteral("id"), id);
    msg.insert(QStringLiteral("error"), err);
    writeJsonLine(msg);
}

void McpServer::writeJsonLine(const QJsonObject &obj)
{
    const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    std::cout.write(line.constData(), line.size());
    std::cout.put('\n');
    std::cout.flush();
}
