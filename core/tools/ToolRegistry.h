#pragma once

#include "core/tools/ITool.h"

#include <QHash>
#include <QList>
#include <QString>

#include <memory>
#include <vector>

namespace nexa {

// Registry of every tool, in registration order (the order the model sees them).
//
// A tool can be withheld from the model for a while — take_screenshot while the user is not
// sharing the screen — with the reason in words: specs() leaves it out, so no request offers it,
// and the Agent answers a call to it with the reason instead of running it. find() still returns
// it, so the fast path can act on the user's own explicit request ("what's on my screen?").
class ToolRegistry {
public:
    // Replaces an existing tool of the same name.
    void add(std::unique_ptr<ITool> tool);
    void remove(const QString &name);       // a connector's tools go when it does
    ITool *find(const QString &name) const;
    bool contains(const QString &name) const { return byName_.contains(name); }
    QList<ToolSpec> specs() const;          // the tools offered to the model
    void cancelAll();

    void withhold(const QString &name, const QString &reason);
    void offer(const QString &name);
    bool isOffered(const QString &name) const { return !withheld_.contains(name); }
    QString withheldReason(const QString &name) const { return withheld_.value(name); }   // empty when offered

private:
    std::vector<std::unique_ptr<ITool>> tools_;
    QHash<QString, ITool *> byName_;
    QHash<QString, QString> withheld_;     // name → why the model is not offered it now
};

} // namespace nexa
