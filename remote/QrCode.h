#pragma once

#include <QImage>
#include <QString>

namespace nexa {

// A QR code for the pairing link shown in Settings → Remote: black modules on white, `scale` pixels
// each, with the four-module quiet zone scanners need around it. libqrencode does the encoding.
// A null image when the text cannot be encoded.
QImage qrCode(const QString &text, int scale = 6);

} // namespace nexa
