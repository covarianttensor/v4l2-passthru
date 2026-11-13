#pragma once

#include <QString>
#include <QList>

struct _GstElement;

struct VideoDeviceInfo {
    QString devicePath;     // e.g. /dev/video0
    QString displayName;    // e.g. VC-003: VC-003 (/dev/video0)
    QString vendorId;       // e.g. 345f
    QString productId;      // e.g. 2131
    QString usbPath;        // e.g. pci-0000:0e:00.3-usb-0:2:1.0
    bool isPreferred = false;
};

struct AudioSourceInfo {
    QString id;             // GStreamer/Pulse/PipeWire identifier
    QString displayName;    // Human-readable name
    QString api;            // "pipewire", "pulse", or "alsa"
    bool isMonitor = false;
    bool isPreferred = false;
};

namespace DeviceDiscovery {

QList<VideoDeviceInfo> enumerateVideoDevices();
QList<AudioSourceInfo> enumerateAudioSources();

// Creates a configured GStreamer audio source element matching the target id
// (or auto-selects the preferred VC-003 device when targetId is empty).
_GstElement *createAudioSourceElement(const QString &targetId, QString *resolvedLabel = nullptr);

// Ensures hardware ALSA capture switch (e.g., "Digital In" on VC-003) is unmuted.
void ensureCaptureCardUnmuted();

} // namespace DeviceDiscovery
