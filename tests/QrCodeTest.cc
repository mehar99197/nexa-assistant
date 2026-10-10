// remote/QrCode: the pairing link drawn for a phone camera — square, with a white quiet zone and
// the finder patterns in three corners.

#include "remote/QrCode.h"

#include <QColor>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

bool dark(const QImage &image, int moduleX, int moduleY, int scale) {
    // The centre of the module, so neighbouring modules cannot blur the answer.
    return qGray(image.pixel(moduleX * scale + scale / 2, moduleY * scale + scale / 2)) < 128;
}

} // namespace

TEST_CASE("A pairing link becomes a square QR code with its quiet zone") {
    const int scale = 5;
    const QImage image = qrCode(u"https://192.168.0.115:7777/#pair=123456"_s, scale);
    REQUIRE_FALSE(image.isNull());
    CHECK(image.width() == image.height());
    CHECK(image.width() % scale == 0);

    const int modules = image.width() / scale;          // including four quiet modules each side
    const int quiet = 4;
    for (int i = 0; i < modules; ++i) {
        CHECK_FALSE(dark(image, i, 0, scale));          // the quiet zone is white all round
        CHECK_FALSE(dark(image, 0, i, scale));
        CHECK_FALSE(dark(image, i, modules - 1, scale));
        CHECK_FALSE(dark(image, modules - 1, i, scale));
    }
    // Finder patterns: a dark corner module top-left, top-right and bottom-left — never bottom-right.
    const int last = modules - quiet - 1;
    CHECK(dark(image, quiet, quiet, scale));
    CHECK(dark(image, last, quiet, scale));
    CHECK(dark(image, quiet, last, scale));
    // …each with a light ring inside its dark border.
    CHECK_FALSE(dark(image, quiet + 1, quiet + 1, scale));
    CHECK(dark(image, quiet + 3, quiet + 3, scale));
}

TEST_CASE("Nothing to encode, nothing drawn") {
    CHECK(qrCode(QString()).isNull());
    CHECK(qrCode(u"x"_s, 0).isNull());
}
