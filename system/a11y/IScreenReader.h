#pragma once

#include "core/Result.h"
#include "system/a11y/ScreenOutline.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace nexa {

// What the screen tools need of the desktop's accessibility bus (Strategy): the applications' trees, an element's
// words, and the acts on an element — its own actions, the keyboard focus, an editable field's text. The real one is
// AtspiScreenReader; the tests use a fake. Every call is asynchronous and reports back on the caller's thread.
class IScreenReader {
public:
    struct TextRequest {
        atspi::NodeRef ref;
        int maxChars{160};
        bool fromEnd{false};        // a terminal's tail
    };
    struct TextSlice {
        QString text;
        int offset{0};
        int total{0};               // characters in the element
    };

    virtual ~IScreenReader() = default;

    virtual bool isAvailable() const = 0;
    virtual QString unavailableReason() const = 0;

    // Every application on the bus with its tree (one GetItems each, or a capped walk for a bridge without a cache).
    virtual void apps(std::function<void(Result<QList<AtspiApp>>)> done) = 0;
    // The words of the elements the outline shows; an element that answers nothing is simply absent.
    virtual void texts(const QList<TextRequest> &requests, std::function<void(QHash<atspi::NodeRef, QString>)> done) = 0;
    // A slice of one element's text, for screen_read.
    virtual void text(const atspi::NodeRef &ref, int offset, int maxChars, std::function<void(Result<TextSlice>)> done) = 0;
    virtual void actions(const atspi::NodeRef &ref, std::function<void(Result<QStringList>)> done) = 0;
    virtual void doAction(const atspi::NodeRef &ref, int index, std::function<void(Result<void>)> done) = 0;
    virtual void grabFocus(const atspi::NodeRef &ref, std::function<void(Result<void>)> done) = 0;
    virtual void setText(const atspi::NodeRef &ref, const QString &text, std::function<void(Result<void>)> done) = 0;
};

} // namespace nexa
