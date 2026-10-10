// system/input/CombinedInputBackend: everything goes to the backend that reaches every window,
// except putting the pointer on a spot, which goes to the one that can.

#include "system/input/CombinedInputBackend.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// Records every call, so the test can see who was asked to do what.
class RecordingBackend : public IInputBackend {
public:
    RecordingBackend(QString label, QStringList *log) : label_(std::move(label)), log_(log) {}

    QString name() const override { return label_; }
    QString reach() const override { return label_ + u" reach"_s; }

    void typeText(const QString &text, Done done) override { record(u"type "_s + text, done); }
    void pressKeys(const QList<int> &keys, Done done) override { record(u"keys %1"_s.arg(keys.size()), done); }
    void mouseMove(int dx, int dy, Done done) override { record(u"move %1,%2"_s.arg(dx).arg(dy), done); }
    void mouseMoveAbs(int x, int y, int w, int h, Done done) override {
        record(u"to %1,%2 of %3x%4"_s.arg(x).arg(y).arg(w).arg(h), done);
    }
    void mouseClick(int button, int count, Done done) override {
        record(u"click %1x%2"_s.arg(button).arg(count), done);
    }
    void mouseScroll(int dx, int dy, Done done) override { record(u"scroll %1,%2"_s.arg(dx).arg(dy), done); }

private:
    void record(const QString &what, const Done &done) {
        log_->append(label_ + u": "_s + what);
        done(QString());
    }

    QString label_;
    QStringList *log_;
};

} // namespace

TEST_CASE("Only placing the pointer goes to the positioner") {
    QStringList log;
    CombinedInputBackend combined(std::make_unique<RecordingBackend>(u"helper"_s, &log),
                                  std::make_unique<RecordingBackend>(u"portal"_s, &log));
    const auto ignore = [](Result<QString>) {};

    combined.typeText(u"hi"_s, ignore);
    combined.pressKeys({29, 31}, ignore);
    combined.mouseMove(5, -5, ignore);
    combined.mouseMoveAbs(400, 300, 1920, 1080, ignore);
    combined.mouseClick(1, 2, ignore);
    combined.mouseScroll(0, -3, ignore);

    CHECK(log == QStringList{
                     u"helper: type hi"_s,
                     u"helper: keys 2"_s,
                     u"helper: move 5,-5"_s,
                     u"portal: to 400,300 of 1920x1080"_s,
                     // The click comes from the helper, and lands where the portal put the pointer.
                     u"helper: click 1x2"_s,
                     u"helper: scroll 0,-3"_s,
                 });
}

TEST_CASE("The combination introduces itself as the backend that does most of the work") {
    QStringList log;
    const CombinedInputBackend combined(std::make_unique<RecordingBackend>(u"helper"_s, &log),
                                        std::make_unique<RecordingBackend>(u"portal"_s, &log));
    CHECK(combined.name() == u"helper"_s);
    CHECK(combined.reach() == u"helper reach"_s);
}
