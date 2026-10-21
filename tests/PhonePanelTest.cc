// The window's Phone panel, with a picture handed in: no phone and no adb are needed to see that it
// shows what it is given, hides the mirror while nobody watches, and pauses on request.

#include "app/PhonePanel.h"
#include "system/android/AdbClient.h"
#include "system/android/AndroidDevice.h"
#include "ui/NexaTheme.h"

#include "QtStringMakers.h"

#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QTest>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// A phone's screen: a status bar, some rows of text and a keyboard, in the phone's proportions.
QImage fakePhoneScreen() {
    QImage image(540, 1200, QImage::Format_RGB32);
    image.fill(QColor(0x12, 0x14, 0x1a));
    QPainter painter(&image);
    painter.fillRect(0, 0, 540, 60, QColor(0x1f, 0x24, 0x2e));
    painter.setPen(Qt::white);
    painter.drawText(QRect(20, 0, 500, 60), Qt::AlignVCenter, u"12:34   ▲ ▮▮▮ 76%"_s);
    for (int row = 0; row < 8; ++row) {
        painter.fillRect(20, 100 + row * 110, 500, 90, QColor(0x2a, 0x30, 0x3c));
        painter.drawText(QRect(40, 100 + row * 110, 460, 90), Qt::AlignVCenter, u"Message %1"_s.arg(row + 1));
    }
    painter.fillRect(0, 1000, 540, 200, QColor(0x1f, 0x24, 0x2e));
    return image;
}

void snapshot(QWidget &widget, const QString &name) {
    const QString directory = qEnvironmentVariable("NEXA_TEST_SCREENSHOTS");
    if (directory.isEmpty()) return;
    QTest::qWait(50);
    widget.grab().save(directory + u'/' + name + u".png"_s);
}

} // namespace

TEST_CASE("The Phone panel shows the picture it is given and pauses on request") {
    AdbClient adb;
    AndroidDevice phone(&adb);
    PhonePanel panel(&phone);
    panel.resize(300, 640);
    CHECK_FALSE(panel.isConnected());   // nothing is connected here, so the mirror never asks adb
    CHECK_FALSE(panel.isPaused());

    panel.show();
    QTest::qWait(30);
    panel.showFrame(fakePhoneScreen(), 900);
    auto *status = panel.findChild<QLabel *>(u"phoneStatus"_s);
    REQUIRE(status != nullptr);
    CHECK(status->text() == u"Live · 0.9 s per frame"_s);
    snapshot(panel, u"phone-panel"_s);

    auto *pause = panel.findChild<QToolButton *>(u"phoneButton"_s);
    REQUIRE(pause != nullptr);
    pause->click();
    CHECK(panel.isPaused());
    CHECK(pause->text() == u"Live"_s);
    pause->click();
    CHECK_FALSE(panel.isPaused());
    CHECK(pause->text() == u"Pause"_s);

    // The sheet sets the colours; without it the dark tokens are the default.
    CHECK(panel.lineColor() == NexaTheme::tokens(ThemeScheme::Dark).line);
}
