// system/android/AdbParsers: what adb and the phone's shell print, read tolerantly. The samples are
// written the way platform-tools 34 and Android 12–14 print them, awkward cases included.

#include "system/android/AdbParsers.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("adb devices -l: every state, USB and Wi-Fi alike") {
    const QString output = uR"(* daemon not running; starting now at tcp:5037
* daemon started successfully
List of devices attached
R58M12345ABC           device usb:1-1 product:a52qnsxx model:SM_A525F device:a52q transport_id:1
192.168.18.50:37135    device product:a52qnsxx model:SM_A525F device:a52q transport_id:2
adb-R58M12345ABC-AbCdEf._adb-tls-connect._tcp device product:a52qnsxx model:SM_A525F device:a52q transport_id:3
0123456789ABCDEF       unauthorized usb:1-2 transport_id:4
emulator-5554          offline transport_id:5
????????????           no permissions (missing udev rules? user is in the plugdev group); see [http://developer.android.com/tools/device.html] usb:1-3 transport_id:6

)"_s;
    const QList<AdbDevice> devices = adb::parseDevices(output);
    REQUIRE(devices.size() == 6);

    CHECK(devices.at(0).serial == u"R58M12345ABC"_s);
    CHECK(devices.at(0).isReady());
    CHECK(devices.at(0).usb);
    CHECK_FALSE(devices.at(0).isWireless());
    CHECK(devices.at(0).model == u"SM_A525F"_s);
    CHECK(devices.at(0).displayModel() == u"SM A525F"_s);
    CHECK(devices.at(0).transportId == u"1"_s);

    CHECK(devices.at(1).isWireless());
    CHECK(devices.at(2).isWireless());          // connected through mDNS
    CHECK(devices.at(3).state == u"unauthorized"_s);
    CHECK_FALSE(devices.at(3).isReady());
    CHECK(devices.at(4).state == u"offline"_s);
    // A state with spaces, and a URL that looks like a key:value field.
    CHECK(devices.at(5).state == u"no permissions"_s);
    CHECK(devices.at(5).usb);
    CHECK(devices.at(5).transportId == u"6"_s);

    CHECK(adb::parseDevices(u"List of devices attached\n\n"_s).isEmpty());
}

TEST_CASE("adb mdns services: pairing and connect offers, with or without the trailing dot") {
    const QString output = u"List of discovered mdns services\n"
                           "adb-R58M12345ABC-AbCdEf\t_adb-tls-connect._tcp\t192.168.18.50:37135\n"
                           "adb-R58M12345ABC-AbCdEf\t_adb-tls-pairing._tcp.\t192.168.18.50:41247\n"
                           "adb-OTHER99-Zz\t_adb._tcp\t192.168.18.61:5555\n"
                           "adb-V6PHONE-Qq\t_adb-tls-connect._tcp\t[fe80::1]:40001\n"
                           "garbage line\n"
                           "adb-BAD-Port\t_adb-tls-connect._tcp\t192.168.18.70:notaport\n"_s;
    const QList<MdnsService> services = adb::parseMdnsServices(output);
    REQUIRE(services.size() == 4);
    CHECK(services.at(0).kind == MdnsService::Kind::Connect);
    CHECK(services.at(0).address() == u"192.168.18.50:37135"_s);
    CHECK(services.at(1).kind == MdnsService::Kind::Pairing);
    CHECK(services.at(1).port == 41247);
    CHECK(services.at(2).kind == MdnsService::Kind::Other);
    CHECK(services.at(3).host == u"fe80::1"_s);
    CHECK(services.at(3).address() == u"[fe80::1]:40001"_s);

    CHECK(adb::serialFromServiceName(u"adb-R58M12345ABC-AbCdEf"_s) == u"R58M12345ABC"_s);
    CHECK(adb::serialFromServiceName(u"adb-R58M12345ABC-AbCdEf._adb-tls-connect._tcp"_s) == u"R58M12345ABC"_s);
    CHECK(adb::serialFromServiceName(u"192.168.18.50:37135"_s).isEmpty());
}

TEST_CASE("wm size: the override wins, because that is what the screen uses") {
    CHECK(adb::parseWmSize(u"Physical size: 1080x2400\n"_s) == QSize(1080, 2400));
    CHECK(adb::parseWmSize(u"Physical size: 1440x3200\nOverride size: 1080x2400\n"_s) == QSize(1080, 2400));
    CHECK_FALSE(adb::parseWmSize(u"cmd: Can't find service: window"_s));
}

TEST_CASE("dumpsys notification: who, what and when, with the noise left out") {
    const QString output = uR"(Current Notification Manager state:
  Notification List:
    NotificationRecord(0x0e6a1c4f: pkg=com.whatsapp user=UserHandle{0} id=1 tag=null importance=4 key=0|com.whatsapp|1|null|10123: Notification(channel=individual_chat_defaults_3 shortcut=null contentView=null vibrate=null sound=null defaults=0x0 flags=0x8 color=0xff25d366 category=msg vis=PRIVATE))
      uid=10123 userId=0
      opPkg=com.whatsapp
      notification=
        extras={
          android.title=String (Ali)
          android.text=SpannableString (Are we still on for tonight?)
          android.showWhen=Boolean (true)
        }
      mCreationTimeMs=1696936742011
    NotificationRecord(0x0d1e2f3a: pkg=com.google.android.gm user=UserHandle{0} id=-1234567 tag=null importance=3 key=0|com.google.android.gm|... : Notification(channel=^sync_channel ...))
      notification=
        extras={
          android.title=String (Invoice (October))
          android.text=SpannableString (Your invoice is ready)
          android.bigText=SpannableString (Your invoice is ready.
Pay by the 20th to avoid fees.)
        }
      mCreationTimeMs=1696936000000
    NotificationRecord(0x01: pkg=android user=UserHandle{0} id=26 tag=null importance=1 key=0|android|26|null|1000: Notification(channel=DEVELOPER ...))
      notification=
        extras={
          android.title=null
          android.text=null
        }
    NotificationRecord(0x02: pkg=com.example.redacted user=UserHandle{0} id=3 tag=null importance=3 key=x: Notification(channel=x))
      notification=
        extras={
          android.title=String [length=5]
          android.text=String (Something happened)
        }
)"_s;
    const QList<PhoneNotification> list = adb::parseNotifications(output);
    REQUIRE(list.size() == 3);       // the empty system one is left out

    CHECK(list.at(0).package == u"com.whatsapp"_s);
    CHECK(list.at(0).title == u"Ali"_s);
    CHECK(list.at(0).text == u"Are we still on for tonight?"_s);
    CHECK(list.at(0).timeMs == 1696936742011);

    // Brackets inside the title, and a long text over two lines that beats the short one.
    CHECK(list.at(1).title == u"Invoice (October)"_s);
    CHECK(list.at(1).text == u"Your invoice is ready.\nPay by the 20th to avoid fees."_s);

    // A redacted title says nothing; the text still counts.
    CHECK(list.at(2).title.isEmpty());
    CHECK(list.at(2).text == u"Something happened"_s);
}

TEST_CASE("content query rows: commas, line breaks and NULL in the values") {
    const QString output = u"Row: 0 address=+923001234567, date=1696936743123, body=Your code is 1234\n"
                           "Row: 1 address=JazzCash, date=1696930000000, body=Payment received, thank you\n"
                           "Row: 2 address=Mom, date=1696920000000, body=First line\n"
                           "second line, still the same message\n"
                           "Row: 3 address=NULL, date=1696910000000, body=\n"
                           "Row: 4 garbage without the columns\n"_s;
    const QList<QStringList> rows =
        adb::parseContentRows(output, {u"address"_s, u"date"_s, u"body"_s});
    REQUIRE(rows.size() == 4);
    CHECK(rows.at(0) == QStringList{u"+923001234567"_s, u"1696936743123"_s, u"Your code is 1234"_s});
    CHECK(rows.at(1).at(2) == u"Payment received, thank you"_s);
    CHECK(rows.at(2).at(2) == u"First line\nsecond line, still the same message"_s);
    CHECK(rows.at(3).at(0).isEmpty());          // NULL
    CHECK(rows.at(3).at(2).isEmpty());

    CHECK(adb::parseContentRows(u"No result found."_s, {u"address"_s}).isEmpty());

    const QList<QStringList> contacts = adb::parseContentRows(
        u"Row: 0 display_name=Khan, Ali, data1=+92 300 1234567\n"_s, {u"display_name"_s, u"data1"_s});
    REQUIRE(contacts.size() == 1);
    CHECK(contacts.at(0).at(0) == u"Khan, Ali"_s);
    CHECK(contacts.at(0).at(1) == u"+92 300 1234567"_s);
}

TEST_CASE("Launcher activities and package lists") {
    const QString activities = u"com.android.chrome/com.google.android.apps.chrome.Main\n"
                               "com.google.android.youtube/.app.honeycomb.Shell$HomeActivity\n"
                               "  Activity #2:\n"
                               "    com.whatsapp/.Main\n"
                               "com.android.chrome/com.google.android.apps.chrome.Main\n"
                               "3 activities found:\n"_s;
    CHECK(adb::parseLauncherComponents(activities)
          == QStringList{u"com.android.chrome/com.google.android.apps.chrome.Main"_s,
                         u"com.google.android.youtube/.app.honeycomb.Shell$HomeActivity"_s, u"com.whatsapp/.Main"_s});

    CHECK(adb::parsePackages(u"package:com.whatsapp\npackage:/data/app/~~x==/com.spotify.music-1/base.apk=com.spotify.music\n"
                             "junk\npackage:com.whatsapp\n"_s)
          == QStringList{u"com.whatsapp"_s, u"com.spotify.music"_s});
}

TEST_CASE("Volume, battery and properties") {
    const std::optional<StreamVolume> volume = adb::parseVolume(u"[V] volume is 7 in range [0..15]"_s);
    REQUIRE(volume);
    CHECK(volume->current == 7);
    CHECK(volume->maximum == 15);
    CHECK_FALSE(adb::parseVolume(u"Unknown command: volume"_s));
    CHECK_FALSE(adb::parseVolume(u"volume is 3 in range [5..5]"_s));      // no range at all

    CHECK(adb::parseBatteryLevel(u"Current Battery Service state:\n  AC powered: false\n  level: 85\n  scale: 100\n"_s) == 85);
    CHECK_FALSE(adb::parseBatteryLevel(u"  scale: 100\n"_s));

    const QHash<QString, QString> properties =
        adb::parseProperties(u"manufacturer=samsung\nmodel=SM-A525F\nrelease=14\nnot a property\nmodel=second\n"_s);
    CHECK(properties.value(u"manufacturer"_s) == u"samsung"_s);
    CHECK(properties.value(u"model"_s) == u"SM-A525F"_s);     // the first one wins
    CHECK(properties.value(u"release"_s) == u"14"_s);
}
