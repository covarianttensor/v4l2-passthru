#include "DeviceDiscovery.h"

#include <alsa/asoundlib.h>
#include <gst/gst.h>
#include <libudev.h>
#include <linux/videodev2.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <QSet>

namespace {

QString safeStr(const char *s)
{
    return s ? QString::fromUtf8(s) : QString();
}

bool matchesPreferredCard(const QString &name, const QString &vid, const QString &pid)
{
    if (vid.compare("345f", Qt::CaseInsensitive) == 0 &&
        pid.compare("2131", Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QString upper = name.toUpper();
    return upper.contains("VC-003") || upper.contains("MACROSILICON");
}

struct ScannedGstAudio {
    AudioSourceInfo info;
    GstDevice *device = nullptr; // owned while scanning
};

QList<ScannedGstAudio> scanGstAudioDevices()
{
    QList<ScannedGstAudio> result;

    GstDeviceMonitor *monitor = gst_device_monitor_new();
    if (!monitor) {
        return result;
    }

    GstCaps *caps = gst_caps_new_empty_simple("audio/x-raw");
    gst_device_monitor_add_filter(monitor, "Audio/Source", caps);
    gst_caps_unref(caps);

    if (!gst_device_monitor_start(monitor)) {
        gst_object_unref(monitor);
        return result;
    }

    GList *devices = gst_device_monitor_get_devices(monitor);

    for (GList *l = devices; l != nullptr; l = l->next) {
        auto *dev = GST_DEVICE(l->data);
        if (!dev) {
            continue;
        }

        GstElement *probeElem = gst_device_create_element(dev, nullptr);
        GstElementFactory *factory = probeElem ? gst_element_get_factory(probeElem) : nullptr;
        const QString factoryName = factory
            ? safeStr(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)))
            : QString();
        if (probeElem) {
            gst_object_unref(probeElem);
        }

        // Only keep PipeWire/PulseAudio sources so we never grab raw ALSA hw:0,0 exclusively.
        if (factoryName != QStringLiteral("pulsesrc") &&
            factoryName != QStringLiteral("pipewiresrc")) {
            continue;
        }

        gchar *rawName = gst_device_get_display_name(dev);
        QString displayName = safeStr(rawName);
        g_free(rawName);

        GstStructure *props = gst_device_get_properties(dev);
        QString nodeName;
        QString vid;
        QString pid;
        QString alsaCardName;
        bool isMonitor = false;

        if (props) {
            nodeName = safeStr(gst_structure_get_string(props, "node.name"));
            if (nodeName.isEmpty()) {
                nodeName = safeStr(gst_structure_get_string(props, "device.name"));
            }
            if (nodeName.isEmpty()) {
                nodeName = safeStr(gst_structure_get_string(props, "object.path"));
            }
            vid = safeStr(gst_structure_get_string(props, "device.vendor.id"));
            pid = safeStr(gst_structure_get_string(props, "device.product.id"));
            alsaCardName = safeStr(gst_structure_get_string(props, "alsa.card_name"));

            const char *devClass = gst_structure_get_string(props, "device.class");
            if (devClass && QString::fromUtf8(devClass).contains("monitor", Qt::CaseInsensitive)) {
                isMonitor = true;
            }
            gst_structure_free(props);
        }

        if (nodeName.endsWith(".monitor", Qt::CaseInsensitive) ||
            displayName.startsWith("Monitor of ", Qt::CaseInsensitive)) {
            isMonitor = true;
        }

        if (isMonitor) {
            continue;
        }

        const QString id = !nodeName.isEmpty() ? nodeName : displayName;
        if (id.isEmpty()) {
            continue;
        }

        // Normalize vendor/product id strings like "0x345f" -> "345f"
        if (vid.startsWith("0x", Qt::CaseInsensitive)) {
            vid = vid.mid(2);
        }
        if (pid.startsWith("0x", Qt::CaseInsensitive)) {
            pid = pid.mid(2);
        }

        const QString api = (factoryName == QStringLiteral("pulsesrc"))
            ? QStringLiteral("pulse")
            : QStringLiteral("pipewire");

        AudioSourceInfo info;
        info.id = id;
        info.displayName = QStringLiteral("%1 [%2]").arg(displayName, api);
        info.api = api;
        info.isMonitor = false;
        info.isPreferred = matchesPreferredCard(displayName + " " + id + " " + alsaCardName, vid, pid);

        result.append({info, static_cast<GstDevice *>(gst_object_ref(dev))});
    }

    g_list_free_full(devices, gst_object_unref);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);

    auto apiRank = [](const QString &api) {
        if (api == QStringLiteral("pulse")) return 0;
        if (api == QStringLiteral("pipewire")) return 1;
        return 2;
    };

    std::stable_sort(result.begin(), result.end(), [&](const ScannedGstAudio &a, const ScannedGstAudio &b) {
        if (a.info.isPreferred != b.info.isPreferred) {
            return a.info.isPreferred > b.info.isPreferred;
        }
        const int ra = apiRank(a.info.api);
        const int rb = apiRank(b.info.api);
        if (ra != rb) {
            return ra < rb;
        }
        return a.info.displayName < b.info.displayName;
    });

    // Deduplicate after sorting so pulsesrc (with explicit low-latency tuning) wins over duplicate pipewiresrc entries.
    QSet<QString> seenIds;
    QList<ScannedGstAudio> deduped;
    deduped.reserve(result.size());
    for (auto &item : result) {
        if (seenIds.contains(item.info.id)) {
            if (item.device) {
                gst_object_unref(item.device);
            }
            continue;
        }
        seenIds.insert(item.info.id);
        deduped.append(item);
    }

    return deduped;
}

} // namespace

namespace DeviceDiscovery {

QList<VideoDeviceInfo> enumerateVideoDevices()
{
    QList<VideoDeviceInfo> result;

    struct udev *udevCtx = udev_new();
    if (!udevCtx) {
        return result;
    }

    struct udev_enumerate *enumerate = udev_enumerate_new(udevCtx);
    udev_enumerate_add_match_subsystem(enumerate, "video4linux");
    udev_enumerate_scan_devices(enumerate);

    struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry *entry = nullptr;

    udev_list_entry_foreach(entry, devices) {
        const char *sysPath = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(udevCtx, sysPath);
        if (!dev) {
            continue;
        }

        const QString devNode = safeStr(udev_device_get_devnode(dev));
        if (devNode.isEmpty()) {
            udev_device_unref(dev);
            continue;
        }

        const QString caps = safeStr(udev_device_get_property_value(dev, "ID_V4L_CAPABILITIES"));
        bool isCapture = caps.contains(":capture:");
        QString capCardName;

        if (!isCapture) {
            const int fd = ::open(devNode.toUtf8().constData(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                struct v4l2_capability vcap{};
                if (::ioctl(fd, VIDIOC_QUERYCAP, &vcap) == 0) {
                    const uint32_t devCaps = (vcap.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? vcap.device_caps
                        : vcap.capabilities;
                    isCapture = (devCaps & V4L2_CAP_VIDEO_CAPTURE) != 0;
                    capCardName = QString::fromUtf8(reinterpret_cast<const char *>(vcap.card));
                }
                ::close(fd);
            }
        }

        if (!isCapture) {
            udev_device_unref(dev);
            continue;
        }

        QString product = safeStr(udev_device_get_property_value(dev, "ID_V4L_PRODUCT"));
        if (product.isEmpty()) {
            product = safeStr(udev_device_get_sysattr_value(dev, "name"));
        }
        if (product.isEmpty()) {
            product = capCardName;
        }
        if (product.isEmpty()) {
            product = safeStr(udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE"));
        }
        if (product.isEmpty()) {
            product = safeStr(udev_device_get_property_value(dev, "ID_MODEL"));
        }

        QString vid = safeStr(udev_device_get_property_value(dev, "ID_VENDOR_ID"));
        QString pid = safeStr(udev_device_get_property_value(dev, "ID_MODEL_ID"));
        if (vid.isEmpty() || pid.isEmpty()) {
            struct udev_device *usbDev =
                udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
            if (usbDev) {
                if (vid.isEmpty()) vid = safeStr(udev_device_get_sysattr_value(usbDev, "idVendor"));
                if (pid.isEmpty()) pid = safeStr(udev_device_get_sysattr_value(usbDev, "idProduct"));
            }
        }

        const QString usbPath = safeStr(udev_device_get_property_value(dev, "ID_PATH"));

        VideoDeviceInfo info;
        info.devicePath = devNode;
        info.vendorId = vid;
        info.productId = pid;
        info.usbPath = usbPath;
        info.isPreferred = matchesPreferredCard(product, vid, pid);
        info.displayName = product.isEmpty()
            ? devNode
            : QStringLiteral("%1 (%2)").arg(product, devNode);

        result.append(info);
        udev_device_unref(dev);
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udevCtx);

    std::stable_sort(result.begin(), result.end(), [](const VideoDeviceInfo &a, const VideoDeviceInfo &b) {
        if (a.isPreferred != b.isPreferred) {
            return a.isPreferred > b.isPreferred;
        }
        return a.devicePath < b.devicePath;
    });

    return result;
}

QList<AudioSourceInfo> enumerateAudioSources()
{
    auto scanned = scanGstAudioDevices();
    QList<AudioSourceInfo> result;
    result.reserve(scanned.size());
    for (auto &item : scanned) {
        result.append(item.info);
        if (item.device) {
            gst_object_unref(item.device);
        }
    }
    return result;
}

GstElement *createAudioSourceElement(const QString &targetId, QString *resolvedLabel)
{
    auto scanned = scanGstAudioDevices();
    GstDevice *chosen = nullptr;
    QString chosenLabel;

    for (const auto &item : scanned) {
        if (!targetId.isEmpty() && item.info.id == targetId) {
            chosen = item.device;
            chosenLabel = item.info.displayName;
            break;
        }
    }

    if (!chosen && !scanned.isEmpty()) {
        // Fallback to preferred (VC-003) or first available source when targetId is empty or missing.
        if (targetId.isEmpty() || scanned.first().info.isPreferred) {
            chosen = scanned.first().device;
            chosenLabel = scanned.first().info.displayName;
        }
    }

    GstElement *elem = nullptr;
    if (chosen) {
        elem = gst_device_create_element(chosen, "asrc");
        if (elem) {
            // Configure low-latency buffering if pulsesrc/pipewiresrc exposes these properties.
            GObjectClass *klass = G_OBJECT_GET_CLASS(elem);
            if (g_object_class_find_property(klass, "latency-time")) {
                g_object_set(elem, "latency-time", static_cast<gint64>(10000), nullptr);
            }
            if (g_object_class_find_property(klass, "buffer-time")) {
                g_object_set(elem, "buffer-time", static_cast<gint64>(25000), nullptr);
            }
            if (g_object_class_find_property(klass, "client-name")) {
                g_object_set(elem, "client-name", "V4L2 Passthru", nullptr);
            }
        }
    }

    for (auto &item : scanned) {
        if (item.device) {
            gst_object_unref(item.device);
        }
    }

    if (resolvedLabel) {
        *resolvedLabel = chosenLabel;
    }
    return elem;
}

void ensureCaptureCardUnmuted()
{
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char *cardName = nullptr;
        if (snd_card_get_name(card, &cardName) < 0 || !cardName) {
            continue;
        }
        const QString name = QString::fromUtf8(cardName);
        std::free(cardName);

        if (!name.contains("VC-003", Qt::CaseInsensitive) &&
            !name.contains("VC003", Qt::CaseInsensitive) &&
            !name.contains("MACROSILICON", Qt::CaseInsensitive)) {
            continue;
        }

        const QByteArray hwName = QStringLiteral("hw:%1").arg(card).toUtf8();
        snd_mixer_t *mixer = nullptr;
        if (snd_mixer_open(&mixer, 0) < 0 || !mixer) {
            continue;
        }

        if (snd_mixer_attach(mixer, hwName.constData()) >= 0 &&
            snd_mixer_selem_register(mixer, nullptr, nullptr) >= 0 &&
            snd_mixer_load(mixer) >= 0) {
            for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mixer);
                 elem != nullptr;
                 elem = snd_mixer_elem_next(elem)) {
                if (snd_mixer_selem_has_capture_switch(elem)) {
                    snd_mixer_selem_set_capture_switch_all(elem, 1);
                }
            }
        }
        snd_mixer_close(mixer);
    }
}

} // namespace DeviceDiscovery
