#pragma once

#include "system/a11y/IScreenReader.h"

#include <QSet>
#include <QStringList>

namespace nexa::test {

// A scripted IScreenReader: the applications it answers with, the words and actions of its elements by path, the
// paths that have gone away, and a log of what was done to which element. Answers at once, on the caller's thread.
class FakeScreenReader : public IScreenReader {
public:
    bool available{true};
    QString reason{QStringLiteral("no accessibility bus in this test")};
    QList<AtspiApp> scripted;              // what apps() answers
    QHash<QString, QString> textByPath;
    QHash<QString, QStringList> actionsByPath;
    QSet<QString> gone;
    QStringList log;
    int appsCalls{0};
    std::function<void()> beforeApps;       // the test changes `scripted` from here

    bool isAvailable() const override { return available; }
    QString unavailableReason() const override { return reason; }

    void apps(std::function<void(Result<QList<AtspiApp>>)> done) override {
        ++appsCalls;
        if (beforeApps) beforeApps();
        done(scripted);
    }

    void texts(const QList<TextRequest> &requests, std::function<void(QHash<atspi::NodeRef, QString>)> done) override {
        QHash<atspi::NodeRef, QString> out;
        for (const TextRequest &request : requests) {
            const auto found = textByPath.constFind(request.ref.path);
            if (found == textByPath.cend()) continue;
            out.insert(request.ref, request.fromEnd ? found->right(request.maxChars) : found->left(request.maxChars));
        }
        done(out);
    }

    void text(const atspi::NodeRef &ref, int offset, int maxChars, std::function<void(Result<TextSlice>)> done) override {
        if (const std::optional<Error> error = missing(ref)) {
            done(*error);
            return;
        }
        const QString whole = textByPath.value(ref.path);
        done(TextSlice{whole.mid(offset, maxChars), offset, int(whole.size())});
    }

    void actions(const atspi::NodeRef &ref, std::function<void(Result<QStringList>)> done) override {
        if (const std::optional<Error> error = missing(ref)) {
            done(*error);
            return;
        }
        done(actionsByPath.value(ref.path));
    }

    void doAction(const atspi::NodeRef &ref, int index, std::function<void(Result<void>)> done) override {
        if (const std::optional<Error> error = missing(ref)) {
            done(*error);
            return;
        }
        log.append(QStringLiteral("doAction %1 %2").arg(ref.path).arg(index));
        done(Result<void>{});
    }

    void grabFocus(const atspi::NodeRef &ref, std::function<void(Result<void>)> done) override {
        if (const std::optional<Error> error = missing(ref)) {
            done(*error);
            return;
        }
        log.append(QStringLiteral("grabFocus %1").arg(ref.path));
        done(Result<void>{});
    }

    void setText(const atspi::NodeRef &ref, const QString &text, std::function<void(Result<void>)> done) override {
        if (const std::optional<Error> error = missing(ref)) {
            done(*error);
            return;
        }
        log.append(QStringLiteral("setText %1 %2").arg(ref.path, text));
        done(Result<void>{});
    }

private:
    std::optional<Error> missing(const atspi::NodeRef &ref) const {
        if (!gone.contains(ref.path)) return std::nullopt;
        return Error{ErrorCode::NotFound, QStringLiteral("That element is gone: take screen_snapshot again.")};
    }
};

} // namespace nexa::test
