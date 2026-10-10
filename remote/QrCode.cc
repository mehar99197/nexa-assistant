#include "remote/QrCode.h"

#include <qrencode.h>

#include <algorithm>
#include <memory>

namespace nexa {

namespace {

constexpr int kQuietZone = 4;

} // namespace

QImage qrCode(const QString &text, int scale) {
    if (text.isEmpty() || scale < 1) return {};
    const QByteArray utf8 = text.toUtf8();
    // Level M: a smudged or angled phone camera still reads it.
    const std::unique_ptr<QRcode, decltype(&QRcode_free)> code(
        QRcode_encodeString(utf8.constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1), &QRcode_free);
    if (!code) return {};

    const int modules = code->width;
    const int side = (modules + 2 * kQuietZone) * scale;
    QImage image(side, side, QImage::Format_Grayscale8);
    image.fill(255);
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if ((code->data[y * modules + x] & 1) == 0) continue;     // bit 0: this module is dark
            for (int dy = 0; dy < scale; ++dy) {
                uchar *row = image.scanLine((y + kQuietZone) * scale + dy);
                std::fill_n(row + (x + kQuietZone) * scale, scale, uchar(0));
            }
        }
    }
    return image;
}

} // namespace nexa
