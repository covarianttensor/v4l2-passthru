#pragma once

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QVideoFrame>

struct EncodedImage {
    QByteArray bytes;
    QString mimeType;
    int width = 0;
    int height = 0;
    bool ok = false;
};

struct OcrResult {
    bool available = false;
    bool ok = false;
    QString text;
    QString error;
};

namespace InspectionEngine {

QString defaultSocketPath();

// Converts a QVideoFrame to an RGB/ARGB QImage, applying optional crop and downscale.
// Crop object accepts { x, y, width, height } in normalized [0..1] coordinates if all
// values <= 1.0 (or pixel coordinates otherwise, or explicit units: "px"|"norm").
QImage frameToImage(const QVideoFrame &frame, const QJsonObject &crop = QJsonObject(), int maxWidth = 0);

// Encodes a QImage as PNG or JPEG.
EncodedImage encodeImage(const QImage &img, const QString &format = QStringLiteral("png"), int quality = 90);

// Downscales an image to a compact grayscale grid for fast visual diffing.
QImage computeLumaGrid(const QImage &img);

// Returns fraction [0.0..1.0] of grid pixels differing by > threshold luma levels.
double computeGridDiff(const QImage &gridA, const QImage &gridB, int pixelDeltaThreshold = 14);

bool isTesseractAvailable();
OcrResult runOcr(const QImage &img, bool invert = false, int psm = 6);

// Builds a JSON payload for a captured frame (image_base64, mimeType, width, height, etc.).
// If ocrSourceImg is valid, OCR runs against it instead of the potentially downscaled img.
QJsonObject buildFramePayload(const QImage &img,
                              quint64 seq,
                              qint64 epochMs,
                              qint64 ageMs,
                              const QString &format,
                              int quality,
                              bool withOcr,
                              bool invertOcr,
                              const QString &savePath,
                              QString *errorOut = nullptr,
                              const QImage &ocrSourceImg = QImage());

} // namespace InspectionEngine
