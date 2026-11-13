#include "CaptureController.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QVariantMap>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

qint64 steadyNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

GstFlowReturn onAppSinkNewSample(GstAppSink *appsink, gpointer userData)
{
    auto *self = static_cast<CaptureController *>(userData);
    if (self) {
        self->processAppSinkSample(appsink);
    }
    return GST_FLOW_OK;
}

GstBusSyncReply onVideoBusSyncMessage(GstBus *, GstMessage *msg, gpointer userData)
{
    auto *self = static_cast<CaptureController *>(userData);
    if (!self) {
        return GST_BUS_PASS;
    }

    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        const QString text = err ? QString::fromUtf8(err->message) : QStringLiteral("GStreamer video error");
        if (err) g_error_free(err);
        g_free(dbg);

        QMetaObject::invokeMethod(self, "onVideoBusError", Qt::QueuedConnection, Q_ARG(QString, text));
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
        QMetaObject::invokeMethod(self, "onVideoBusError", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("Video stream ended (EOS)")));
    }

    return GST_BUS_PASS;
}

GstBusSyncReply onAudioBusSyncMessage(GstBus *, GstMessage *msg, gpointer userData)
{
    auto *self = static_cast<CaptureController *>(userData);
    if (!self) {
        return GST_BUS_PASS;
    }

    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        const QString text = err ? QString::fromUtf8(err->message) : QStringLiteral("GStreamer audio error");
        if (err) g_error_free(err);
        g_free(dbg);

        QMetaObject::invokeMethod(self, "onAudioBusError", Qt::QueuedConnection, Q_ARG(QString, text));
    }

    return GST_BUS_PASS;
}

void teardownPipeline(GstElement *&pipeline)
{
    if (!pipeline) {
        return;
    }

    GstBus *bus = gst_element_get_bus(pipeline);
    if (bus) {
        gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
        gst_object_unref(bus);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    pipeline = nullptr;
}

} // namespace

CaptureController::CaptureController(QObject *parent)
    : QObject(parent)
{
    loadSettings();

    m_watchdogTimer.setInterval(250);
    connect(&m_watchdogTimer, &QTimer::timeout, this, &CaptureController::onWatchdogTick);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &CaptureController::onReconnectTimer);

    refreshDevices();
}

CaptureController::~CaptureController()
{
    stop();
    std::lock_guard<std::mutex> lock(m_sinkMutex);
    m_videoSink = nullptr;
}

QVideoSink *CaptureController::videoSink() const
{
    std::lock_guard<std::mutex> lock(m_sinkMutex);
    return m_videoSink;
}

void CaptureController::setVideoSink(QVideoSink *sink)
{
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        if (m_videoSink == sink) {
            return;
        }
        m_videoSink = sink;
    }
    emit videoSinkChanged();
}

QVariantList CaptureController::videoDevices() const
{
    QVariantList list;
    QVariantMap autoItem;
    autoItem.insert(QStringLiteral("id"), QStringLiteral(""));
    autoItem.insert(QStringLiteral("label"), QStringLiteral("Auto (VC-003)"));
    list.append(autoItem);

    for (const auto &dev : m_videoDevices) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), dev.devicePath);
        item.insert(QStringLiteral("label"), dev.displayName);
        list.append(item);
    }
    return list;
}

QVariantList CaptureController::audioSources() const
{
    QVariantList list;
    QVariantMap autoItem;
    autoItem.insert(QStringLiteral("id"), QStringLiteral(""));
    autoItem.insert(QStringLiteral("label"), QStringLiteral("Auto (VC-003)"));
    list.append(autoItem);

    for (const auto &src : m_audioSources) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), src.id);
        item.insert(QStringLiteral("label"), src.displayName);
        list.append(item);
    }
    return list;
}

QString CaptureController::selectedVideoDevice() const { return m_selectedVideoDevice; }

void CaptureController::setSelectedVideoDevice(const QString &devicePath)
{
    if (m_selectedVideoDevice == devicePath) {
        return;
    }
    m_selectedVideoDevice = devicePath;
    saveSettings();
    emit selectedVideoDeviceChanged();
    if (m_wantRunning) {
        reconnectNow();
    }
}

QString CaptureController::selectedAudioSource() const { return m_selectedAudioSource; }

void CaptureController::setSelectedAudioSource(const QString &sourceId)
{
    if (m_selectedAudioSource == sourceId) {
        return;
    }
    m_selectedAudioSource = sourceId;
    saveSettings();
    emit selectedAudioSourceChanged();
    if (m_wantRunning && m_audioEnabled) {
        stopAudioPipeline();
        startAudioPipeline();
    }
}

QString CaptureController::pixelFormat() const { return m_pixelFormat; }

void CaptureController::setPixelFormat(const QString &format)
{
    const QString normalized = (format.compare("MJPG", Qt::CaseInsensitive) == 0)
        ? QStringLiteral("MJPG")
        : QStringLiteral("YUYV");
    if (m_pixelFormat == normalized) {
        return;
    }
    m_pixelFormat = normalized;
    saveSettings();
    emit settingsChanged();
    if (m_wantRunning) {
        reconnectNow();
    }
}

int CaptureController::fps() const { return m_fps; }

void CaptureController::setFps(int fps)
{
    const int clamped = (fps == 30 || fps == 50 || fps == 60) ? fps : 60;
    if (m_fps == clamped) {
        return;
    }
    m_fps = clamped;
    saveSettings();
    emit settingsChanged();
    if (m_wantRunning) {
        reconnectNow();
    }
}

bool CaptureController::audioEnabled() const { return m_audioEnabled; }

void CaptureController::setAudioEnabled(bool enabled)
{
    if (m_audioEnabled == enabled) {
        return;
    }
    m_audioEnabled = enabled;
    saveSettings();
    emit audioSettingsChanged();

    if (!m_wantRunning) {
        return;
    }
    if (m_audioEnabled) {
        startAudioPipeline();
    } else {
        stopAudioPipeline();
    }
}

bool CaptureController::muted() const { return m_muted; }

void CaptureController::setMuted(bool muted)
{
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    saveSettings();
    applyAudioVolumeSettings();
    emit audioSettingsChanged();
}

double CaptureController::volume() const { return m_volume; }

void CaptureController::setVolume(double volume)
{
    const double clamped = std::clamp(volume, 0.0, 1.5);
    if (qFuzzyCompare(m_volume, clamped)) {
        return;
    }
    m_volume = clamped;
    saveSettings();
    applyAudioVolumeSettings();
    emit audioSettingsChanged();
}

bool CaptureController::autoReconnect() const { return m_autoReconnect; }

void CaptureController::setAutoReconnect(bool enabled)
{
    if (m_autoReconnect == enabled) {
        return;
    }
    m_autoReconnect = enabled;
    saveSettings();
    emit settingsChanged();
}

bool CaptureController::streaming() const { return m_streaming; }
bool CaptureController::reconnecting() const { return m_reconnecting; }
QString CaptureController::statusText() const { return m_statusText; }
QString CaptureController::audioStatus() const { return m_audioStatus; }
QString CaptureController::errorDetail() const { return m_errorDetail; }
QString CaptureController::activeResolution() const { return m_activeResolution; }
double CaptureController::measuredFps() const { return m_measuredFps; }
int CaptureController::reconnectCount() const { return m_reconnectCount; }

void CaptureController::refreshDevices()
{
    m_videoDevices = DeviceDiscovery::enumerateVideoDevices();
    m_audioSources = DeviceDiscovery::enumerateAudioSources();
    emit devicesChanged();
}

void CaptureController::start()
{
    m_wantRunning = true;
    m_reconnectTimer.stop();
    m_backoffStep = 0;

    refreshDevices();
    setStatus(false, false, QStringLiteral("Connecting..."));

    if (!startVideoPipeline()) {
        if (m_autoReconnect) {
            scheduleReconnect(m_errorDetail.isEmpty()
                ? QStringLiteral("Waiting for capture device...")
                : m_errorDetail);
        }
        return;
    }

    if (m_audioEnabled) {
        startAudioPipeline();
    }

    m_lastTelemetryMs = steadyNowMs();
    m_watchdogTimer.start();
}

void CaptureController::stop()
{
    m_wantRunning = false;
    m_watchdogTimer.stop();
    m_reconnectTimer.stop();
    m_backoffStep = 0;

    stopVideoPipeline();
    stopAudioPipeline();

    m_measuredFps = 0.0;
    emit telemetryChanged();
    setStatus(false, false, QStringLiteral("Stopped"));
}

void CaptureController::reconnectNow()
{
    m_wantRunning = true;
    m_reconnectTimer.stop();
    m_watchdogTimer.stop();
    m_backoffStep = 0;

    stopVideoPipeline();
    stopAudioPipeline();

    start();
}

void CaptureController::loadSettings()
{
    QSettings s(QStringLiteral("v4l2-passthru"), QStringLiteral("v4l2-passthru"));
    m_selectedVideoDevice = s.value(QStringLiteral("videoDevice"), QString()).toString();
    m_selectedAudioSource = s.value(QStringLiteral("audioSource"), QString()).toString();
    m_pixelFormat = s.value(QStringLiteral("pixelFormat"), QStringLiteral("YUYV")).toString();
    if (m_pixelFormat != QStringLiteral("MJPG")) {
        m_pixelFormat = QStringLiteral("YUYV");
    }
    m_fps = s.value(QStringLiteral("fps"), 60).toInt();
    if (m_fps != 30 && m_fps != 50 && m_fps != 60) {
        m_fps = 60;
    }
    m_audioEnabled = s.value(QStringLiteral("audioEnabled"), true).toBool();
    m_muted = s.value(QStringLiteral("muted"), false).toBool();
    m_volume = std::clamp(s.value(QStringLiteral("volume"), 1.0).toDouble(), 0.0, 1.5);
    m_autoReconnect = s.value(QStringLiteral("autoReconnect"), true).toBool();
}

void CaptureController::saveSettings() const
{
    QSettings s(QStringLiteral("v4l2-passthru"), QStringLiteral("v4l2-passthru"));
    s.setValue(QStringLiteral("videoDevice"), m_selectedVideoDevice);
    s.setValue(QStringLiteral("audioSource"), m_selectedAudioSource);
    s.setValue(QStringLiteral("pixelFormat"), m_pixelFormat);
    s.setValue(QStringLiteral("fps"), m_fps);
    s.setValue(QStringLiteral("audioEnabled"), m_audioEnabled);
    s.setValue(QStringLiteral("muted"), m_muted);
    s.setValue(QStringLiteral("volume"), m_volume);
    s.setValue(QStringLiteral("autoReconnect"), m_autoReconnect);
}

bool CaptureController::startVideoPipeline()
{
    stopVideoPipeline();

    QString targetDev = m_selectedVideoDevice;
    if (!targetDev.isEmpty() && !QFileInfo::exists(targetDev)) {
        targetDev.clear();
    }

    if (targetDev.isEmpty()) {
        if (m_videoDevices.isEmpty()) {
            setStatus(false, m_reconnecting, QStringLiteral("No capture device found"),
                      QStringLiteral("Plug in VC-003 HDMI capture card"));
            return false;
        }
        targetDev = m_videoDevices.first().devicePath;
    }

    const QString pipelineDesc = (m_pixelFormat == QStringLiteral("MJPG"))
        ? QStringLiteral(
            "v4l2src device=\"%1\" io-mode=mmap ! "
            "image/jpeg,width=1920,height=1080,framerate=%2/1 ! "
            "jpegdec ! video/x-raw,format=I420 ! "
            "appsink name=vsink emit-signals=true max-buffers=1 drop=true sync=false")
              .arg(targetDev)
              .arg(m_fps)
        : QStringLiteral(
            "v4l2src device=\"%1\" io-mode=mmap ! "
            "video/x-raw,format=YUY2,width=1920,height=1080,framerate=%2/1 ! "
            "appsink name=vsink emit-signals=true max-buffers=1 drop=true sync=false")
              .arg(targetDev)
              .arg(m_fps);

    GError *err = nullptr;
    m_videoPipeline = gst_parse_launch(pipelineDesc.toUtf8().constData(), &err);
    if (!m_videoPipeline || err) {
        const QString msg = err ? QString::fromUtf8(err->message) : QStringLiteral("Failed to build video pipeline");
        if (err) g_error_free(err);
        teardownPipeline(m_videoPipeline);
        setStatus(false, m_reconnecting, QStringLiteral("Pipeline error"), msg);
        return false;
    }

    GstElement *sinkElem = gst_bin_get_by_name(GST_BIN(m_videoPipeline), "vsink");
    if (!sinkElem) {
        teardownPipeline(m_videoPipeline);
        setStatus(false, m_reconnecting, QStringLiteral("Pipeline error"), QStringLiteral("Missing appsink"));
        return false;
    }

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = onAppSinkNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sinkElem), &callbacks, this, nullptr);
    gst_object_unref(sinkElem);

    GstBus *bus = gst_element_get_bus(m_videoPipeline);
    if (bus) {
        gst_bus_set_sync_handler(bus, onVideoBusSyncMessage, this, nullptr);
        gst_object_unref(bus);
    }

    m_lastFrameSteadyMs.store(0);
    m_frameCounter.store(0);
    m_pipelineStartMs.store(steadyNowMs());

    const GstStateChangeReturn ret = gst_element_set_state(m_videoPipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        teardownPipeline(m_videoPipeline);
        setStatus(false, m_reconnecting, QStringLiteral("Failed to open capture device"),
                  QStringLiteral("Could not start %1").arg(targetDev));
        return false;
    }

    qInfo() << "Started video capture:" << targetDev << m_pixelFormat << m_fps << "fps";
    return true;
}

void CaptureController::stopVideoPipeline()
{
    teardownPipeline(m_videoPipeline);
    m_lastFrameSteadyMs.store(0);
    m_frameCounter.store(0);
}

bool CaptureController::startAudioPipeline()
{
    stopAudioPipeline();

    DeviceDiscovery::ensureCaptureCardUnmuted();

    QString resolvedLabel;
    GstElement *src = DeviceDiscovery::createAudioSourceElement(m_selectedAudioSource, &resolvedLabel);
    if (!src) {
        m_audioStatus = QStringLiteral("Audio: No PipeWire/Pulse input found");
        emit stateChanged();
        return false;
    }

    m_audioPipeline = gst_pipeline_new("v4l2-passthru-audio");
    GstElement *conv = gst_element_factory_make("audioconvert", "aconv");
    GstElement *resamp = gst_element_factory_make("audioresample", "aresamp");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "acaps");
    GstElement *vol = gst_element_factory_make("volume", "avol");

    GstElement *sink = gst_element_factory_make("pulsesink", "asink");
    bool isPulseSink = (sink != nullptr);
    if (!sink) {
        sink = gst_element_factory_make("pipewiresink", "asink");
    }

    if (!m_audioPipeline || !conv || !resamp || !capsfilter || !vol || !sink) {
        if (src) gst_object_unref(src);
        if (conv) gst_object_unref(conv);
        if (resamp) gst_object_unref(resamp);
        if (capsfilter) gst_object_unref(capsfilter);
        if (vol) gst_object_unref(vol);
        if (sink) gst_object_unref(sink);
        if (m_audioPipeline) {
            gst_object_unref(m_audioPipeline);
            m_audioPipeline = nullptr;
        }
        return false;
    }

    GstCaps *caps = gst_caps_new_simple("audio/x-raw",
                                        "format", G_TYPE_STRING, "S16LE",
                                        "channels", G_TYPE_INT, 2,
                                        "rate", G_TYPE_INT, 48000,
                                        nullptr);
    g_object_set(capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(vol, "mute", m_muted ? TRUE : FALSE, "volume", m_volume, nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);

    if (isPulseSink) {
        g_object_set(sink,
                     "latency-time", static_cast<gint64>(10000),
                     "buffer-time", static_cast<gint64>(25000),
                     "client-name", "V4L2 Passthru",
                     nullptr);
    } else {
        g_object_set(sink, "client-name", "V4L2 Passthru", nullptr);
    }

    GstStructure *streamProps = gst_structure_new("props",
                                                  "application.name", G_TYPE_STRING, "V4L2 Passthru",
                                                  "media.role", G_TYPE_STRING, "game",
                                                  nullptr);
    if (streamProps) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "stream-properties")) {
            g_object_set(sink, "stream-properties", streamProps, nullptr);
        }
        gst_structure_free(streamProps);
    }

    gst_bin_add_many(GST_BIN(m_audioPipeline), src, conv, resamp, capsfilter, vol, sink, nullptr);
    if (!gst_element_link_many(src, conv, resamp, capsfilter, vol, sink, nullptr)) {
        teardownPipeline(m_audioPipeline);
        return false;
    }

    m_audioVolumeElem = static_cast<GstElement *>(gst_object_ref(vol));

    GstBus *bus = gst_element_get_bus(m_audioPipeline);
    if (bus) {
        gst_bus_set_sync_handler(bus, onAudioBusSyncMessage, this, nullptr);
        gst_object_unref(bus);
    }

    if (gst_element_set_state(m_audioPipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stopAudioPipeline();
        m_audioStatus = QStringLiteral("Audio: Failed to start");
        emit stateChanged();
        return false;
    }

    m_audioStatus = QStringLiteral("Audio: %1").arg(resolvedLabel);
    emit stateChanged();
    qInfo() << "Started audio passthrough:" << resolvedLabel;
    return true;
}

void CaptureController::stopAudioPipeline()
{
    if (m_audioVolumeElem) {
        gst_object_unref(m_audioVolumeElem);
        m_audioVolumeElem = nullptr;
    }
    teardownPipeline(m_audioPipeline);
    m_audioStatus = m_audioEnabled ? QStringLiteral("Audio: Stopped") : QStringLiteral("Audio: Disabled");
    emit stateChanged();
}

void CaptureController::applyAudioVolumeSettings()
{
    if (m_audioVolumeElem) {
        g_object_set(m_audioVolumeElem,
                     "mute", m_muted ? TRUE : FALSE,
                     "volume", m_volume,
                     nullptr);
    }
}

void CaptureController::processAppSinkSample(GstAppSink *appsink)
{
    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) {
        return;
    }

    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!caps || !buffer) {
        gst_sample_unref(sample);
        return;
    }

    GstVideoInfo vinfo;
    if (!gst_video_info_from_caps(&vinfo, caps)) {
        gst_sample_unref(sample);
        return;
    }

    GstVideoFrame gframe;
    if (!gst_video_frame_map(&gframe, &vinfo, buffer, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return;
    }

    const int width = GST_VIDEO_FRAME_WIDTH(&gframe);
    const int height = GST_VIDEO_FRAME_HEIGHT(&gframe);
    const GstVideoFormat gstFmt = GST_VIDEO_FRAME_FORMAT(&gframe);

    QVideoFrameFormat::PixelFormat qtFmt = QVideoFrameFormat::Format_Invalid;
    if (gstFmt == GST_VIDEO_FORMAT_YUY2) {
        qtFmt = QVideoFrameFormat::Format_YUYV;
    } else if (gstFmt == GST_VIDEO_FORMAT_I420) {
        qtFmt = QVideoFrameFormat::Format_YUV420P;
    }

    if (qtFmt != QVideoFrameFormat::Format_Invalid) {
        QVideoFrameFormat fmt(QSize(width, height), qtFmt);
        QVideoFrame qframe(fmt);

        if (qframe.map(QVideoFrame::WriteOnly)) {
            const int planes = static_cast<int>(GST_VIDEO_FRAME_N_PLANES(&gframe));
            for (int p = 0; p < planes; ++p) {
                const auto *src = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&gframe, p));
                uchar *dst = qframe.bits(p);
                const int srcStride = GST_VIDEO_FRAME_PLANE_STRIDE(&gframe, p);
                const int dstStride = qframe.bytesPerLine(p);
                const int rowBytes = std::min(srcStride, dstStride);
                const int planeHeight = GST_VIDEO_FRAME_COMP_HEIGHT(&gframe, p);

                for (int y = 0; y < planeHeight; ++y) {
                    std::memcpy(dst + y * dstStride, src + y * srcStride, static_cast<size_t>(rowBytes));
                }
            }
            qframe.unmap();

            if (GST_BUFFER_PTS_IS_VALID(buffer)) {
                const qint64 us = static_cast<qint64>(GST_BUFFER_PTS(buffer) / 1000);
                qframe.setStartTime(us);
            }

            m_activeWidth.store(width);
            m_activeHeight.store(height);
            m_lastFrameSteadyMs.store(steadyNowMs());
            m_frameCounter.fetch_add(1);

            std::lock_guard<std::mutex> lock(m_sinkMutex);
            if (m_videoSink) {
                m_videoSink->setVideoFrame(qframe);
            }
        }
    }

    gst_video_frame_unmap(&gframe);
    gst_sample_unref(sample);
}

void CaptureController::onWatchdogTick()
{
    if (!m_wantRunning) {
        return;
    }

    const qint64 now = steadyNowMs();
    const qint64 lastFrame = m_lastFrameSteadyMs.load();

    // Update FPS telemetry every ~500ms.
    const qint64 dtMs = now - m_lastTelemetryMs;
    if (dtMs >= 500) {
        const int frames = m_frameCounter.exchange(0);
        m_measuredFps = (dtMs > 0) ? (static_cast<double>(frames) * 1000.0 / static_cast<double>(dtMs)) : 0.0;
        m_lastTelemetryMs = now;

        const QString res = QStringLiteral("%1x%2").arg(m_activeWidth.load()).arg(m_activeHeight.load());
        if (m_activeResolution != res) {
            m_activeResolution = res;
        }
        emit telemetryChanged();
    }

    if (m_reconnecting) {
        return;
    }

    if (lastFrame > 0) {
        if (!m_streaming) {
            m_backoffStep = 0;
            setStatus(true, false, QStringLiteral("Streaming"));
        } else if (now - lastFrame > 1200) {
            scheduleReconnect(QStringLiteral("Frame timeout (>1.2s stall)"));
        }
    } else if (m_videoPipeline && (now - m_pipelineStartMs.load() > 3000)) {
        scheduleReconnect(QStringLiteral("Timed out waiting for first frame"));
    }
}

void CaptureController::onVideoBusError(const QString &message)
{
    if (!m_wantRunning || m_reconnecting) {
        return;
    }
    scheduleReconnect(message);
}

void CaptureController::onAudioBusError(const QString &message)
{
    qWarning() << "Audio bus error:" << message;
    if (!m_wantRunning || !m_audioEnabled) {
        return;
    }
    // Keep video running; quietly tear down failed audio until next recovery or retry.
    stopAudioPipeline();
    m_audioStatus = QStringLiteral("Audio error: %1").arg(message);
    emit stateChanged();
}

void CaptureController::scheduleReconnect(const QString &reason)
{
    if (!m_wantRunning) {
        return;
    }

    qWarning() << "Scheduling capture reconnect:" << reason;

    // Immediately close V4L2 and audio handles so udev/kernel can reset cleanly.
    stopVideoPipeline();
    stopAudioPipeline();

    m_measuredFps = 0.0;
    emit telemetryChanged();

    if (!m_autoReconnect) {
        m_watchdogTimer.stop();
        setStatus(false, false, QStringLiteral("Disconnected"), reason);
        return;
    }

    ++m_reconnectCount;
    static constexpr int kBackoffMs[] = { 450, 900, 1500, 2000 };
    const int delay = kBackoffMs[std::min(m_backoffStep, 3)];
    ++m_backoffStep;

    setStatus(false, true,
              QStringLiteral("Reconnecting (#%1)...").arg(m_reconnectCount),
              reason);

    m_reconnectTimer.start(delay);
}

void CaptureController::onReconnectTimer()
{
    if (!m_wantRunning) {
        return;
    }

    refreshDevices();
    if (!startVideoPipeline()) {
        scheduleReconnect(m_errorDetail.isEmpty()
            ? QStringLiteral("Waiting for VC-003 to re-enumerate...")
            : m_errorDetail);
        return;
    }

    if (m_audioEnabled) {
        startAudioPipeline();
    }

    m_lastTelemetryMs = steadyNowMs();
    if (!m_watchdogTimer.isActive()) {
        m_watchdogTimer.start();
    }
}

void CaptureController::setStatus(bool streaming, bool reconnecting, const QString &status, const QString &detail)
{
    m_streaming = streaming;
    m_reconnecting = reconnecting;
    m_statusText = status;
    m_errorDetail = detail;
    emit stateChanged();
}
