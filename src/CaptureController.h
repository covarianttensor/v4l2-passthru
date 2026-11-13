#pragma once

#include "DeviceDiscovery.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVideoSink>
#include <QQmlEngine>

#include <atomic>
#include <mutex>

struct _GstElement;
struct _GstAppSink;

class CaptureController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVideoSink *videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)

    Q_PROPERTY(QVariantList videoDevices READ videoDevices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList audioSources READ audioSources NOTIFY devicesChanged)
    Q_PROPERTY(QString selectedVideoDevice READ selectedVideoDevice WRITE setSelectedVideoDevice NOTIFY selectedVideoDeviceChanged)
    Q_PROPERTY(QString selectedAudioSource READ selectedAudioSource WRITE setSelectedAudioSource NOTIFY selectedAudioSourceChanged)

    Q_PROPERTY(QString pixelFormat READ pixelFormat WRITE setPixelFormat NOTIFY settingsChanged)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY settingsChanged)

    Q_PROPERTY(bool audioEnabled READ audioEnabled WRITE setAudioEnabled NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY audioSettingsChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY audioSettingsChanged)
    Q_PROPERTY(bool autoReconnect READ autoReconnect WRITE setAutoReconnect NOTIFY settingsChanged)

    Q_PROPERTY(bool streaming READ streaming NOTIFY stateChanged)
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString audioStatus READ audioStatus NOTIFY stateChanged)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY stateChanged)
    Q_PROPERTY(QString activeResolution READ activeResolution NOTIFY telemetryChanged)
    Q_PROPERTY(double measuredFps READ measuredFps NOTIFY telemetryChanged)
    Q_PROPERTY(int reconnectCount READ reconnectCount NOTIFY stateChanged)

public:
    explicit CaptureController(QObject *parent = nullptr);
    ~CaptureController() override;

    QVideoSink *videoSink() const;
    void setVideoSink(QVideoSink *sink);

    QVariantList videoDevices() const;
    QVariantList audioSources() const;

    QString selectedVideoDevice() const;
    void setSelectedVideoDevice(const QString &devicePath);

    QString selectedAudioSource() const;
    void setSelectedAudioSource(const QString &sourceId);

    QString pixelFormat() const;
    void setPixelFormat(const QString &format);

    int fps() const;
    void setFps(int fps);

    bool audioEnabled() const;
    void setAudioEnabled(bool enabled);

    bool muted() const;
    void setMuted(bool muted);

    double volume() const;
    void setVolume(double volume);

    bool autoReconnect() const;
    void setAutoReconnect(bool enabled);

    bool streaming() const;
    bool reconnecting() const;
    QString statusText() const;
    QString audioStatus() const;
    QString errorDetail() const;
    QString activeResolution() const;
    double measuredFps() const;
    int reconnectCount() const;

    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void reconnectNow();

    // Called from GStreamer appsink streaming thread.
    void processAppSinkSample(_GstAppSink *appsink);

signals:
    void videoSinkChanged();
    void devicesChanged();
    void selectedVideoDeviceChanged();
    void selectedAudioSourceChanged();
    void settingsChanged();
    void audioSettingsChanged();
    void stateChanged();
    void telemetryChanged();

private slots:
    void onWatchdogTick();
    void onReconnectTimer();
    void onVideoBusError(const QString &message);
    void onAudioBusError(const QString &message);

private:
    void loadSettings();
    void saveSettings() const;

    bool startVideoPipeline();
    void stopVideoPipeline();

    bool startAudioPipeline();
    void stopAudioPipeline();
    void applyAudioVolumeSettings();

    void scheduleReconnect(const QString &reason);
    void setStatus(bool streaming, bool reconnecting, const QString &status, const QString &detail = QString());

    QList<VideoDeviceInfo> m_videoDevices;
    QList<AudioSourceInfo> m_audioSources;

    QString m_selectedVideoDevice; // empty = auto-select preferred VC-003
    QString m_selectedAudioSource; // empty = auto-select preferred VC-003
    QString m_pixelFormat = QStringLiteral("YUYV");
    int m_fps = 60;
    bool m_audioEnabled = true;
    bool m_muted = false;
    double m_volume = 1.0;
    bool m_autoReconnect = true;

    bool m_wantRunning = false;
    bool m_streaming = false;
    bool m_reconnecting = false;
    QString m_statusText = QStringLiteral("Stopped");
    QString m_audioStatus = QStringLiteral("Audio: Stopped");
    QString m_errorDetail;
    QString m_activeResolution = QStringLiteral("1920x1080");
    double m_measuredFps = 0.0;
    int m_reconnectCount = 0;
    int m_backoffStep = 0;

    QVideoSink *m_videoSink = nullptr;
    mutable std::mutex m_sinkMutex;

    _GstElement *m_videoPipeline = nullptr;
    _GstElement *m_audioPipeline = nullptr;
    _GstElement *m_audioVolumeElem = nullptr;

    std::atomic<qint64> m_lastFrameSteadyMs{0};
    std::atomic<qint64> m_pipelineStartMs{0};
    std::atomic<int> m_frameCounter{0};
    std::atomic<int> m_activeWidth{1920};
    std::atomic<int> m_activeHeight{1080};

    QTimer m_watchdogTimer;
    QTimer m_reconnectTimer;
    qint64 m_lastTelemetryMs = 0;
};
