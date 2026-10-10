#include "app/ActionLogDock.h"

#include <QAction>
#include <QHeaderView>
#include <QJsonDocument>
#include <QTime>
#include <QTreeWidget>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kMaxRows = 300;
constexpr qsizetype kMaxArgumentsShown = 120;
constexpr int kIdRole = Qt::UserRole + 1;

enum Column { Time, Source, Action, Outcome, Duration, Arguments, ColumnCount };

} // namespace

ActionLogDock::ActionLogDock(QWidget *parent) : QDockWidget(tr("Actions"), parent), tree_(new QTreeWidget(this)) {
    setObjectName(u"actionLog"_s);
    tree_->setColumnCount(ColumnCount);
    tree_->setHeaderLabels({tr("Time"), tr("From"), tr("Action"), tr("Result"), tr("ms"), tr("Details")});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setWordWrap(false);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(Arguments, QHeaderView::Interactive);
    tree_->header()->setSectionResizeMode(Outcome, QHeaderView::Stretch);
    tree_->header()->resizeSection(Arguments, 220);

    auto *clear = new QAction(tr("Clear"), tree_);
    connect(clear, &QAction::triggered, this, [this] {
        tree_->clear();
        items_.clear();
    });
    tree_->setContextMenuPolicy(Qt::ActionsContextMenu);
    tree_->addAction(clear);
    setWidget(tree_);
}

void ActionLogDock::actionStarted(quint64 id, const QString &tool, const QJsonObject &arguments, const QString &source) {
    QString details = QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    if (details == u"{}") details.clear();
    auto *item = new QTreeWidgetItem;
    item->setData(Time, kIdRole, id);
    item->setText(Time, QTime::currentTime().toString(u"HH:mm:ss"_s));
    item->setText(Source, source);
    item->setText(Action, tool);
    item->setText(Arguments, details.size() > kMaxArgumentsShown ? details.left(kMaxArgumentsShown) + u"…"_s : details);
    item->setToolTip(Arguments, QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented)));
    item->setText(Outcome, tr("running…"));
    tree_->insertTopLevelItem(0, item);
    items_.insert(id, item);
    trim();
}

void ActionLogDock::actionFinished(quint64 id, bool ok, const QString &summary, qint64 elapsedMs) {
    QTreeWidgetItem *item = items_.take(id);
    if (!item) return;
    item->setText(Outcome, (ok ? u"✓ "_s : u"✗ "_s) + summary);
    item->setToolTip(Outcome, summary);
    item->setText(Duration, QString::number(elapsedMs));
    item->setTextAlignment(Duration, Qt::AlignRight | Qt::AlignVCenter);
    if (!ok) item->setForeground(Outcome, QColor(0xe5, 0x53, 0x4b));
}

void ActionLogDock::trim() {
    while (tree_->topLevelItemCount() > kMaxRows) {
        QTreeWidgetItem *oldest = tree_->takeTopLevelItem(tree_->topLevelItemCount() - 1);
        items_.remove(oldest->data(Time, kIdRole).toULongLong());
        delete oldest;
    }
}

} // namespace nexa
