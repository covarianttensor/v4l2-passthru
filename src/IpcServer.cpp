#include "IpcServer.h"

#include "CaptureController.h"
#include "InspectionEngine.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

QJsonObject makeError(const QString &message)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("ok"), false);
    obj.insert(QStringLiteral("error"), message);
    return obj;
}

QJsonObject makeOk(QJsonObject data = QJsonObject())
{
    data.insert(QStringLiteral("ok"), true);
    return data;
}

bool waitForBytesAvailable(QLocalSocket *socket, int needed, int timeoutMs)
{
    const qint64 deadline = CaptureController::steadyClockNowMs() + timeoutMs;
    while (socket->bytesAvailable() < needed) {
        const qint64 remaining = deadline - CaptureController::steadyClockNowMs();
        if (remaining <= 0) {
            return false;
        }
        if (!socket->waitForReadyRead(static_cast<int>(std::min<qint64>(remaining, 1000)))) {
            if (socket->state() != QLocalSocket::ConnectedState) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

InspectionService::InspectionService(CaptureController *controller, QObject *parent)
    : QObject(parent)
    , m_controller(controller)
{
}

void InspectionService::execute(const QJsonObject &request, Callback callback)
{
    if (!m_controller) {
        callback(makeError(QStringLiteral("CaptureController unavailable")));
        return;
    }

    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QStringLiteral("status")) {
        handleStatus(params, std::move(callback));
    } else if (method == QStringLiteral("grab_frame")) {
        handleGrabFrame(params, std::move(callback));
    } else if (method == QStringLiteral("ocr_screen")) {
        handleOcrScreen(params, std::move(callback));
    } else if (method == QStringLiteral("wait_for_change")) {
        handleWaitForChange(params, std::move(callback));
    } else if (method == QStringLiteral("wait_for_signal")) {
        handleWaitForSignal(params, std::move(callback));
    } else if (method == QStringLiteral("capture_burst")) {
        handleCaptureBurst(params, std::move(callback));
    } else if (method == QStringLiteral("control")) {
        handleControl(params, std::move(callback));
    } else {
        callback(makeError(QStringLiteral("Unknown method: %1").arg(method)));
    }
}

void InspectionService::handleStatus(const QJsonObject &, Callback callback)
{
    QJsonObject status = m_controller->statusSnapshot();
    status.insert(QStringLiteral("tesseractAvailable"), InspectionEngine::isTesseractAvailable());
    callback(makeOk(status));
}

void InspectionService::handleGrabFrame(const QJsonObject &params, Callback callback)
{
    const FrameSnapshot snap = m_controller->latestFrameSnapshot();
    if (!snap.frame.isValid()) {
        callback(makeError(QStringLiteral("No video frame captured yet (status: %1)")
                               .arg(m_controller->statusText())));
        return;
    }

    const QJsonObject crop = params.value(QStringLiteral("crop")).toObject();
    const int maxWidth = params.value(QStringLiteral("maxWidth")).toInt(0);
    const QString format = params.value(QStringLiteral("format")).toString(QStringLiteral("png"));
    const int quality = params.value(QStringLiteral("quality")).toInt(90);
    const bool withOcr = params.value(QStringLiteral("ocr")).toBool(false);
    const bool invertOcr = params.value(QStringLiteral("invertOcr")).toBool(false);
    const QString savePath = params.value(QStringLiteral("savePath")).toString();

    const QImage fullImg = InspectionEngine::frameToImage(snap.frame, crop, 0);
    if (fullImg.isNull()) {
        callback(makeError(QStringLiteral("Failed to convert video frame to image")));
        return;
    }
    const QImage img = (maxWidth > 0 && fullImg.width() > maxWidth)
        ? fullImg.scaledToWidth(maxWidth, Qt::SmoothTransformation)
        : fullImg;

    const qint64 ageMs = (snap.steadyMs > 0)
        ? (CaptureController::steadyClockNowMs() - snap.steadyMs)
        : -1;

    QString err;
    QJsonObject payload = InspectionEngine::buildFramePayload(
        img, snap.seq, snap.epochMs, ageMs, format, quality, withOcr, invertOcr, savePath, &err, fullImg);
    if (payload.isEmpty()) {
        callback(makeError(err));
        return;
    }

    callback(makeOk(payload));
}

void InspectionService::handleOcrScreen(const QJsonObject &params, Callback callback)
{
    const FrameSnapshot snap = m_controller->latestFrameSnapshot();
    if (!snap.frame.isValid()) {
        callback(makeError(QStringLiteral("No video frame captured yet (status: %1)")
                               .arg(m_controller->statusText())));
        return;
    }

    const QJsonObject crop = params.value(QStringLiteral("crop")).toObject();
    const bool invert = params.value(QStringLiteral("invert")).toBool(false);
    const int psm = params.value(QStringLiteral("psm")).toInt(6);

    const QImage img = InspectionEngine::frameToImage(snap.frame, crop, 0);
    if (img.isNull()) {
        callback(makeError(QStringLiteral("Failed to convert video frame to image")));
        return;
    }

    const OcrResult ocr = InspectionEngine::runOcr(img, invert, psm);
    QJsonObject res;
    res.insert(QStringLiteral("frameSeq"), static_cast<qint64>(snap.seq));
    res.insert(QStringLiteral("width"), img.width());
    res.insert(QStringLiteral("height"), img.height());
    res.insert(QStringLiteral("available"), ocr.available);
    res.insert(QStringLiteral("ocrOk"), ocr.ok);
    res.insert(QStringLiteral("text"), ocr.text);
    if (!ocr.ok) {
        res.insert(QStringLiteral("ocrError"), ocr.error);
        if (!ocr.available) {
            callback(makeError(ocr.error));
            return;
        }
    }

    callback(makeOk(res));
}

void InspectionService::handleWaitForChange(const QJsonObject &params, Callback callback)
{
    const int timeoutMs = std::clamp(params.value(QStringLiteral("timeoutMs")).toInt(15000), 250, 60000);
    const int pollIntervalMs = std::clamp(params.value(QStringLiteral("pollIntervalMs")).toInt(200), 50, 2000);
    const double threshold = std::clamp(params.value(QStringLiteral("changeThreshold")).toDouble(0.015), 0.001, 1.0);
    const int settleMs = std::clamp(params.value(QStringLiteral("settleMs")).toInt(400), 0, 5000);
    const QJsonObject crop = params.value(QStringLiteral("crop")).toObject();
    const int maxWidth = params.value(QStringLiteral("maxWidth")).toInt(0);
    const QString format = params.value(QStringLiteral("format")).toString(QStringLiteral("png"));
    const int quality = params.value(QStringLiteral("quality")).toInt(90);
    const bool withOcr = params.value(QStringLiteral("ocr")).toBool(false);
    const bool invertOcr = params.value(QStringLiteral("invertOcr")).toBool(false);
    const QString savePath = params.value(QStringLiteral("savePath")).toString();

    struct State {
        qint64 startMs = 0;
        quint64 baselineSeq = 0;
        QImage baselineGrid;
        bool hasBaseline = false;
        double triggerDiff = 0.0;
        bool settledPending = false;
        QTimer timer;
    };

    auto state = std::make_shared<State>();
    state->startMs = CaptureController::steadyClockNowMs();

    const FrameSnapshot initial = m_controller->latestFrameSnapshot();
    if (initial.frame.isValid()) {
        const QImage img = InspectionEngine::frameToImage(initial.frame, crop, 0);
        if (!img.isNull()) {
            state->baselineGrid = InspectionEngine::computeLumaGrid(img);
            state->baselineSeq = initial.seq;
            state->hasBaseline = !state->baselineGrid.isNull();
        }
    }

    auto *ctx = new QObject(this);
    auto finish = [this, ctx, state, callback, crop, maxWidth, format, quality, withOcr, invertOcr, savePath](bool changed, double diffScore) {
        state->timer.stop();
        const qint64 elapsed = CaptureController::steadyClockNowMs() - state->startMs;
        const FrameSnapshot snap = m_controller->latestFrameSnapshot();

        QJsonObject res;
        res.insert(QStringLiteral("changed"), changed);
        res.insert(QStringLiteral("elapsedMs"), elapsed);
        res.insert(QStringLiteral("diffScore"), diffScore);

        if (snap.frame.isValid()) {
            const QImage fullImg = InspectionEngine::frameToImage(snap.frame, crop, 0);
            if (!fullImg.isNull()) {
                const QImage img = (maxWidth > 0 && fullImg.width() > maxWidth)
                    ? fullImg.scaledToWidth(maxWidth, Qt::SmoothTransformation)
                    : fullImg;
                const qint64 ageMs = (snap.steadyMs > 0)
                    ? (CaptureController::steadyClockNowMs() - snap.steadyMs)
                    : -1;
                QJsonObject framePayload = InspectionEngine::buildFramePayload(
                    img, snap.seq, snap.epochMs, ageMs, format, quality, withOcr, invertOcr, savePath, nullptr, fullImg);
                for (auto it = framePayload.begin(); it != framePayload.end(); ++it) {
                    res.insert(it.key(), it.value());
                }
            }
        }

        ctx->deleteLater();
        callback(makeOk(res));
    };

    connect(&state->timer, &QTimer::timeout, ctx, [this, state, finish, timeoutMs, threshold, settleMs, crop]() {
        if (state->settledPending) {
            finish(true, state->triggerDiff);
            return;
        }

        const qint64 now = CaptureController::steadyClockNowMs();
        const FrameSnapshot snap = m_controller->latestFrameSnapshot();

        if (snap.frame.isValid() && snap.seq != state->baselineSeq) {
            const QImage img = InspectionEngine::frameToImage(snap.frame, crop, 0);
            const QImage grid = InspectionEngine::computeLumaGrid(img);
            if (!state->hasBaseline) {
                state->triggerDiff = 1.0;
                if (settleMs > 0) {
                    state->settledPending = true;
                    state->timer.start(settleMs);
                    return;
                }
                finish(true, state->triggerDiff);
                return;
            }

            const double diff = InspectionEngine::computeGridDiff(state->baselineGrid, grid);
            if (diff >= threshold) {
                state->triggerDiff = diff;
                if (settleMs > 0) {
                    state->settledPending = true;
                    state->timer.start(settleMs);
                    return;
                }
                finish(true, diff);
                return;
            }
        }

        if (now - state->startMs >= timeoutMs) {
            finish(false, 0.0);
        }
    });

    state->timer.start(pollIntervalMs);
}

void InspectionService::handleWaitForSignal(const QJsonObject &params, Callback callback)
{
    const QString target = params.value(QStringLiteral("state")).toString(QStringLiteral("streaming"));
    const bool wantStreaming = (target != QStringLiteral("lost"));
    const int timeoutMs = std::clamp(params.value(QStringLiteral("timeoutMs")).toInt(15000), 250, 60000);

    const qint64 startMs = CaptureController::steadyClockNowMs();
    if (m_controller->streaming() == wantStreaming) {
        QJsonObject res = m_controller->statusSnapshot();
        res.insert(QStringLiteral("matched"), true);
        res.insert(QStringLiteral("elapsedMs"), 0);
        callback(makeOk(res));
        return;
    }

    auto *ctx = new QObject(this);
    auto *timer = new QTimer(ctx);
    timer->setInterval(150);

    connect(timer, &QTimer::timeout, ctx, [this, ctx, timer, callback, wantStreaming, startMs, timeoutMs]() {
        const qint64 elapsed = CaptureController::steadyClockNowMs() - startMs;
        if (m_controller->streaming() == wantStreaming || elapsed >= timeoutMs) {
            timer->stop();
            QJsonObject res = m_controller->statusSnapshot();
            res.insert(QStringLiteral("matched"), m_controller->streaming() == wantStreaming);
            res.insert(QStringLiteral("elapsedMs"), elapsed);
            ctx->deleteLater();
            callback(makeOk(res));
        }
    });

    timer->start();
}

void InspectionService::handleCaptureBurst(const QJsonObject &params, Callback callback)
{
    const int durationMs = std::clamp(params.value(QStringLiteral("durationMs")).toInt(8000), 200, 30000);
    const int intervalMs = std::clamp(params.value(QStringLiteral("intervalMs")).toInt(350), 100, 5000);
    const bool onlyChanged = params.value(QStringLiteral("onlyChanged")).toBool(true);
    const double threshold = std::clamp(params.value(QStringLiteral("changeThreshold")).toDouble(0.01), 0.001, 1.0);
    const int maxFrames = std::clamp(params.value(QStringLiteral("maxFrames")).toInt(10), 1, 20);
    const int maxWidth = params.value(QStringLiteral("maxWidth")).toInt(1280);
    const QString format = params.value(QStringLiteral("format")).toString(QStringLiteral("jpeg"));
    const int quality = params.value(QStringLiteral("quality")).toInt(85);
    const bool withOcr = params.value(QStringLiteral("ocr")).toBool(false);
    const bool invertOcr = params.value(QStringLiteral("invertOcr")).toBool(false);
    const QString outputDir = params.value(QStringLiteral("outputDir")).toString();

    if (!outputDir.isEmpty()) {
        QDir().mkpath(outputDir);
    }

    struct BurstState {
        qint64 startMs = 0;
        quint64 lastSeq = 0;
        QImage lastGrid;
        int samplesChecked = 0;
        QJsonArray frames;
        QTimer timer;
    };

    auto state = std::make_shared<BurstState>();
    state->startMs = CaptureController::steadyClockNowMs();

    auto *ctx = new QObject(this);

    auto sampleNow = [this, state, onlyChanged, threshold, maxFrames, maxWidth, format, quality, withOcr, invertOcr, outputDir]() {
        const FrameSnapshot snap = m_controller->latestFrameSnapshot();
        ++state->samplesChecked;
        if (!snap.frame.isValid() || snap.seq == state->lastSeq) {
            return;
        }

        const QImage fullImg = InspectionEngine::frameToImage(snap.frame, QJsonObject(), 0);
        if (fullImg.isNull()) {
            return;
        }

        const QImage grid = InspectionEngine::computeLumaGrid(fullImg);
        double diff = 1.0;
        if (!state->lastGrid.isNull()) {
            diff = InspectionEngine::computeGridDiff(state->lastGrid, grid);
            if (onlyChanged && diff < threshold) {
                return;
            }
        }

        state->lastSeq = snap.seq;
        state->lastGrid = grid;

        QImage outImg = (maxWidth > 0 && fullImg.width() > maxWidth)
            ? fullImg.scaledToWidth(maxWidth, Qt::SmoothTransformation)
            : fullImg;

        QString savePath;
        if (!outputDir.isEmpty()) {
            const QString ext = (format.compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0)
                ? QStringLiteral("png")
                : QStringLiteral("jpg");
            savePath = QStringLiteral("%1/frame_%2_seq%3.%4")
                           .arg(outputDir)
                           .arg(state->frames.size(), 3, 10, QLatin1Char('0'))
                           .arg(snap.seq)
                           .arg(ext);
        }

        const qint64 offsetMs = CaptureController::steadyClockNowMs() - state->startMs;
        const qint64 ageMs = (snap.steadyMs > 0)
            ? (CaptureController::steadyClockNowMs() - snap.steadyMs)
            : -1;

        QJsonObject frameObj = InspectionEngine::buildFramePayload(
            outImg, snap.seq, snap.epochMs, ageMs, format, quality, withOcr, invertOcr, savePath, nullptr, fullImg);
        if (!frameObj.isEmpty()) {
            frameObj.insert(QStringLiteral("tOffsetMs"), offsetMs);
            frameObj.insert(QStringLiteral("diffScore"), diff);
            state->frames.append(frameObj);
        }
    };

    auto finish = [ctx, state, callback, outputDir]() {
        state->timer.stop();
        const qint64 elapsed = CaptureController::steadyClockNowMs() - state->startMs;

        QJsonObject res;
        res.insert(QStringLiteral("elapsedMs"), elapsed);
        res.insert(QStringLiteral("samplesChecked"), state->samplesChecked);
        res.insert(QStringLiteral("capturedCount"), state->frames.size());
        res.insert(QStringLiteral("frames"), state->frames);

        if (!outputDir.isEmpty()) {
            QJsonObject manifest = res;
            QJsonArray slimFrames;
            for (const QJsonValue &val : state->frames) {
                QJsonObject f = val.toObject();
                f.remove(QStringLiteral("imageBase64"));
                slimFrames.append(f);
            }
            manifest.insert(QStringLiteral("frames"), slimFrames);

            QFile mf(outputDir + QStringLiteral("/manifest.json"));
            if (mf.open(QIODevice::WriteOnly)) {
                mf.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
                mf.close();
                res.insert(QStringLiteral("manifestPath"), QFileInfo(mf).absoluteFilePath());
            }
        }

        ctx->deleteLater();
        callback(makeOk(res));
    };

    // Grab an initial baseline sample right away.
    sampleNow();
    if (state->frames.size() >= maxFrames) {
        finish();
        return;
    }

    connect(&state->timer, &QTimer::timeout, ctx, [state, sampleNow, finish, durationMs, maxFrames]() {
        sampleNow();
        const qint64 elapsed = CaptureController::steadyClockNowMs() - state->startMs;
        if (state->frames.size() >= maxFrames || elapsed >= durationMs) {
            finish();
        }
    });

    state->timer.start(intervalMs);
}

void InspectionService::handleControl(const QJsonObject &params, Callback callback)
{
    const QString action = params.value(QStringLiteral("action")).toString();

    if (action == QStringLiteral("reconnect")) {
        m_controller->reconnectNow();
    } else if (action == QStringLiteral("refresh_devices")) {
        m_controller->refreshDevices();
    } else if (action == QStringLiteral("set_format")) {
        const QString fmt = params.value(QStringLiteral("pixelFormat")).toString();
        if (fmt.isEmpty()) {
            callback(makeError(QStringLiteral("pixelFormat is required for set_format")));
            return;
        }
        m_controller->setPixelFormat(fmt);
    } else if (action == QStringLiteral("set_fps")) {
        const int fps = params.value(QStringLiteral("fps")).toInt(0);
        if (fps != 30 && fps != 50 && fps != 60) {
            callback(makeError(QStringLiteral("fps must be 30, 50, or 60")));
            return;
        }
        m_controller->setFps(fps);
    } else if (action == QStringLiteral("set_resolution")) {
        const QString mode = params.value(QStringLiteral("resolutionMode")).toString();
        if (mode != QStringLiteral("1080p")
            && mode != QStringLiteral("720p")
            && mode != QStringLiteral("auto")) {
            callback(makeError(QStringLiteral("resolutionMode must be 1080p, 720p, or auto")));
            return;
        }
        m_controller->setResolutionMode(mode);
    } else if (action == QStringLiteral("set_device")) {
        const QString devicePath = params.value(QStringLiteral("videoDevice")).toString();
        m_controller->setSelectedVideoDevice(devicePath);
    } else {
        callback(makeError(QStringLiteral("Unknown control action: %1").arg(action)));
        return;
    }

    QJsonObject res = m_controller->statusSnapshot();
    res.insert(QStringLiteral("action"), action);
    callback(makeOk(res));
}

IpcServer::IpcServer(CaptureController *controller, const QString &socketPath, QObject *parent)
    : QObject(parent)
    , m_controller(controller)
    , m_service(new InspectionService(controller, this))
    , m_server(new QLocalServer(this))
    , m_socketPath(socketPath)
{
    connect(m_server, &QLocalServer::newConnection, this, &IpcServer::onNewConnection);
}

IpcServer::~IpcServer()
{
    if (m_server && m_server->isListening()) {
        m_server->close();
        QLocalServer::removeServer(m_socketPath);
    }
}

QString IpcServer::socketPath() const
{
    return m_socketPath;
}

bool IpcServer::start()
{
    // Probe whether an active instance is already listening before removing a stale socket file.
    QLocalSocket probe;
    probe.connectToServer(m_socketPath);
    if (probe.waitForConnected(150)) {
        probe.disconnectFromServer();
        qWarning() << "Another v4l2-passthru IPC server is already listening on" << m_socketPath;
        return false;
    }

    QLocalServer::removeServer(m_socketPath);
    if (!m_server->listen(m_socketPath)) {
        qWarning() << "Failed to start IPC server on" << m_socketPath << ":" << m_server->errorString();
        return false;
    }

    qInfo() << "Inspection IPC server listening on" << m_socketPath;
    return true;
}

bool IpcServer::writeFramedJson(QLocalSocket *socket, const QJsonObject &obj, int timeoutMs)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState) {
        return false;
    }

    const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray packet(4 + body.size(), Qt::Uninitialized);
    qToBigEndian(static_cast<quint32>(body.size()), reinterpret_cast<uchar *>(packet.data()));
    std::memcpy(packet.data() + 4, body.constData(), static_cast<size_t>(body.size()));

    socket->write(packet);
    if (timeoutMs <= 0) {
        return socket->flush();
    }
    return socket->waitForBytesWritten(timeoutMs);
}

bool IpcServer::readFramedJsonBlocking(QLocalSocket *socket, QJsonObject *out, int timeoutMs)
{
    if (!socket || !out) {
        return false;
    }

    if (!waitForBytesAvailable(socket, 4, timeoutMs)) {
        return false;
    }

    const QByteArray lenBytes = socket->read(4);
    if (lenBytes.size() != 4) {
        return false;
    }

    const quint32 len = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(lenBytes.constData()));
    if (len == 0 || len > 64u * 1024u * 1024u) {
        return false;
    }

    if (!waitForBytesAvailable(socket, static_cast<int>(len), timeoutMs)) {
        return false;
    }

    const QByteArray payload = socket->read(static_cast<qint64>(len));
    if (payload.size() != static_cast<qint64>(len)) {
        return false;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    *out = doc.object();
    return true;
}

void IpcServer::onNewConnection()
{
    while (QLocalSocket *client = m_server->nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        connect(client, &QLocalSocket::readyRead, this, [this, client, buffer]() {
            handleClientData(client, buffer.get());
        });
        connect(client, &QLocalSocket::disconnected, client, &QLocalSocket::deleteLater);
    }
}

void IpcServer::handleClientData(QLocalSocket *client, QByteArray *buffer)
{
    buffer->append(client->readAll());

    while (buffer->size() >= 4) {
        const quint32 len = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer->constData()));
        if (len == 0 || len > 16u * 1024u * 1024u) {
            client->disconnectFromServer();
            return;
        }
        if (buffer->size() < 4 + static_cast<qsizetype>(len)) {
            return;
        }

        const QByteArray payload = buffer->mid(4, static_cast<qsizetype>(len));
        buffer->remove(0, 4 + static_cast<qsizetype>(len));

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            writeFramedJson(client, makeError(QStringLiteral("Invalid JSON request")));
            continue;
        }

        QPointer<QLocalSocket> safeClient(client);
        m_service->execute(doc.object(), [safeClient](const QJsonObject &response) {
            if (!safeClient || safeClient->state() != QLocalSocket::ConnectedState) {
                return;
            }
            writeFramedJson(safeClient.data(), response, 0);
        });
    }
}
