#include "InspectionEngine.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRect>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace InspectionEngine {

QString defaultSocketPath()
{
    const QByteArray envPath = qgetenv("V4L2_PASSTHRU_SOCKET");
    if (!envPath.isEmpty()) {
        return QString::fromUtf8(envPath);
    }

    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtimeDir.isEmpty()) {
        return runtimeDir + QStringLiteral("/v4l2-passthru.sock");
    }

    return QStringLiteral("/tmp/v4l2-passthru-%1.sock").arg(static_cast<qulonglong>(::getuid()));
}

static QRect resolveCropRect(const QJsonObject &crop, const QSize &size)
{
    if (crop.isEmpty() || !size.isValid()) {
        return QRect();
    }

    const double x = crop.value(QStringLiteral("x")).toDouble(0.0);
    const double y = crop.value(QStringLiteral("y")).toDouble(0.0);
    const double w = crop.value(QStringLiteral("width")).toDouble(0.0);
    const double h = crop.value(QStringLiteral("height")).toDouble(0.0);
    if (w <= 0.0 || h <= 0.0) {
        return QRect();
    }

    const QString units = crop.value(QStringLiteral("units")).toString();
    const bool isNorm = (units == QStringLiteral("norm"))
        || (units.isEmpty() && x >= 0.0 && y >= 0.0 && x <= 1.0 && y <= 1.0 && w <= 1.0 && h <= 1.0);

    QRect rect;
    if (isNorm) {
        rect = QRect(
            static_cast<int>(std::lround(x * size.width())),
            static_cast<int>(std::lround(y * size.height())),
            static_cast<int>(std::lround(w * size.width())),
            static_cast<int>(std::lround(h * size.height())));
    } else {
        rect = QRect(static_cast<int>(std::lround(x)),
                     static_cast<int>(std::lround(y)),
                     static_cast<int>(std::lround(w)),
                     static_cast<int>(std::lround(h)));
    }

    return rect.intersected(QRect(QPoint(0, 0), size));
}

QImage frameToImage(const QVideoFrame &frame, const QJsonObject &crop, int maxWidth)
{
    if (!frame.isValid()) {
        return QImage();
    }

    QImage img = frame.toImage();
    if (img.isNull()) {
        return QImage();
    }

    const QRect cropRect = resolveCropRect(crop, img.size());
    if (cropRect.isValid() && !cropRect.isEmpty()) {
        img = img.copy(cropRect);
    }

    if (maxWidth > 0 && img.width() > maxWidth) {
        img = img.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    }

    return img;
}

EncodedImage encodeImage(const QImage &img, const QString &format, int quality)
{
    EncodedImage out;
    if (img.isNull()) {
        return out;
    }

    const bool useJpeg = (format.compare(QStringLiteral("jpg"), Qt::CaseInsensitive) == 0)
        || (format.compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) == 0);
    const char *qtFmt = useJpeg ? "JPEG" : "PNG";
    const int clampedQuality = useJpeg ? std::clamp(quality, 10, 100) : -1;

    QBuffer buf(&out.bytes);
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, qtFmt, clampedQuality)) {
        return out;
    }

    out.mimeType = useJpeg ? QStringLiteral("image/jpeg") : QStringLiteral("image/png");
    out.width = img.width();
    out.height = img.height();
    out.ok = true;
    return out;
}

QImage computeLumaGrid(const QImage &img)
{
    if (img.isNull()) {
        return QImage();
    }
    return img.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::FastTransformation)
              .convertToFormat(QImage::Format_Grayscale8);
}

double computeGridDiff(const QImage &gridA, const QImage &gridB, int pixelDeltaThreshold)
{
    if (gridA.isNull() || gridB.isNull() || gridA.size() != gridB.size()) {
        return 1.0;
    }

    const int w = gridA.width();
    const int h = gridA.height();
    const int total = w * h;
    if (total <= 0) {
        return 1.0;
    }

    int changed = 0;
    for (int y = 0; y < h; ++y) {
        const uchar *rowA = gridA.constScanLine(y);
        const uchar *rowB = gridB.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (std::abs(static_cast<int>(rowA[x]) - static_cast<int>(rowB[x])) > pixelDeltaThreshold) {
                ++changed;
            }
        }
    }

    return static_cast<double>(changed) / static_cast<double>(total);
}

bool isTesseractAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty();
}

OcrResult runOcr(const QImage &img, bool invert, int psm)
{
    OcrResult res;
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("tesseract"));
    res.available = !exe.isEmpty();
    if (!res.available) {
        res.error = QStringLiteral("tesseract executable not found in PATH");
        return res;
    }

    if (img.isNull()) {
        res.error = QStringLiteral("Input image is empty");
        return res;
    }

    QImage prepared = img;
    if (invert) {
        prepared = prepared.convertToFormat(QImage::Format_RGB32);
        prepared.invertPixels();
    }

    const EncodedImage png = encodeImage(prepared, QStringLiteral("png"), 100);
    if (!png.ok) {
        res.error = QStringLiteral("Failed to encode PNG for OCR");
        return res;
    }

    const int clampedPsm = std::clamp(psm, 0, 13);
    QProcess proc;
    proc.start(exe, {
        QStringLiteral("stdin"),
        QStringLiteral("stdout"),
        QStringLiteral("--psm"),
        QString::number(clampedPsm),
        QStringLiteral("-l"),
        QStringLiteral("eng")
    });

    if (!proc.waitForStarted(2000)) {
        res.error = QStringLiteral("Failed to start tesseract");
        return res;
    }

    proc.write(png.bytes);
    proc.closeWriteChannel();

    if (!proc.waitForFinished(8000)) {
        proc.kill();
        res.error = QStringLiteral("tesseract timed out");
        return res;
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        res.error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (res.error.isEmpty()) {
            res.error = QStringLiteral("tesseract failed with exit code %1").arg(proc.exitCode());
        }
        return res;
    }

    res.ok = true;
    res.text = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    return res;
}

QJsonObject buildFramePayload(const QImage &img,
                              quint64 seq,
                              qint64 epochMs,
                              qint64 ageMs,
                              const QString &format,
                              int quality,
                              bool withOcr,
                              bool invertOcr,
                              const QString &savePath,
                              QString *errorOut,
                              const QImage &ocrSourceImg)
{
    const EncodedImage encoded = encodeImage(img, format, quality);
    if (!encoded.ok) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to encode captured frame");
        }
        return QJsonObject();
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("frameSeq"), static_cast<qint64>(seq));
    obj.insert(QStringLiteral("epochMs"), epochMs);
    obj.insert(QStringLiteral("ageMs"), ageMs);
    obj.insert(QStringLiteral("width"), encoded.width);
    obj.insert(QStringLiteral("height"), encoded.height);
    obj.insert(QStringLiteral("mimeType"), encoded.mimeType);
    obj.insert(QStringLiteral("imageBase64"), QString::fromLatin1(encoded.bytes.toBase64()));

    if (!savePath.isEmpty()) {
        const QFileInfo fi(savePath);
        QDir().mkpath(fi.absolutePath());
        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(encoded.bytes);
            file.close();
            obj.insert(QStringLiteral("savedPath"), fi.absoluteFilePath());
        } else {
            obj.insert(QStringLiteral("saveError"), file.errorString());
        }
    }

    if (withOcr) {
        const QImage &ocrInput = !ocrSourceImg.isNull() ? ocrSourceImg : img;
        const OcrResult ocr = runOcr(ocrInput, invertOcr, 6);
        QJsonObject ocrObj;
        ocrObj.insert(QStringLiteral("available"), ocr.available);
        ocrObj.insert(QStringLiteral("ok"), ocr.ok);
        if (ocr.ok) {
            ocrObj.insert(QStringLiteral("text"), ocr.text);
        } else {
            ocrObj.insert(QStringLiteral("error"), ocr.error);
        }
        obj.insert(QStringLiteral("ocr"), ocrObj);
    }

    return obj;
}

} // namespace InspectionEngine
