#pragma once

#include "system/a11y/IScreenReader.h"

#include <QObject>
#include <QThreadPool>

namespace nexa {

class AtspiBus;

// The real IScreenReader: AT-SPI2 over QtDBus. Every call runs its blocking D-Bus round trips on one worker thread
// (QtDBus connections may be used from any thread) and reports back on the thread it was called from, so nothing
// waits on the GUI thread. An application's tree comes from its org.a11y.atspi.Cache in one call; a bridge without
// one is walked through GetChildren, capped. An element that no longer exists answers NotFound in words.
class AtspiScreenReader : public QObject, public IScreenReader {
    Q_OBJECT
public:
    explicit AtspiScreenReader(AtspiBus *bus, QObject *parent = nullptr);
    ~AtspiScreenReader() override;

    bool isAvailable() const override;
    QString unavailableReason() const override;
    void apps(std::function<void(Result<QList<AtspiApp>>)> done) override;
    void texts(const QList<TextRequest> &requests, std::function<void(QHash<atspi::NodeRef, QString>)> done) override;
    void text(const atspi::NodeRef &ref, int offset, int maxChars, std::function<void(Result<TextSlice>)> done) override;
    void actions(const atspi::NodeRef &ref, std::function<void(Result<QStringList>)> done) override;
    void doAction(const atspi::NodeRef &ref, int index, std::function<void(Result<void>)> done) override;
    void grabFocus(const atspi::NodeRef &ref, std::function<void(Result<void>)> done) override;
    void setText(const atspi::NodeRef &ref, const QString &text, std::function<void(Result<void>)> done) override;

private:
    // Runs `job` on the worker and `done(result)` back here.
    template <typename T>
    void run(std::function<T()> job, std::function<void(T)> done);

    AtspiBus *bus_;
    QThreadPool pool_;
};

} // namespace nexa
