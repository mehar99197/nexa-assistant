#pragma once

#include <QDockWidget>
#include <QHash>
#include <QJsonObject>

class QTreeWidget;
class QTreeWidgetItem;

namespace nexa {

// Live list of every action (tool call) with its source, arguments, result and duration,
// newest first. Fed by ToolExecutor's signals, so fast-path and agent actions look the same.
class ActionLogDock : public QDockWidget {
    Q_OBJECT
public:
    explicit ActionLogDock(QWidget *parent = nullptr);

    void actionStarted(quint64 id, const QString &tool, const QJsonObject &arguments, const QString &source);
    void actionFinished(quint64 id, bool ok, const QString &summary, qint64 elapsedMs);

private:
    void trim();

    QTreeWidget *tree_;
    QHash<quint64, QTreeWidgetItem *> items_;
};

} // namespace nexa
