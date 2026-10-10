#include "app/ActionLogDock.h"

#include "ui/NexaTheme.h"

#include <QAction>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kMaxRows = 300;
constexpr int kPadX = 14;
constexpr int kPadY = 10;
constexpr int kLineGap = 4;

enum Role {
    ToolRole = Qt::UserRole + 1,
    SourceRole,
    ArgumentsRole,
    ResultRole,
    ElapsedRole,
    OkRole,
    RunningRole,
    IdRole,
};

// The arguments as the card shows them: one value alone, several as name=value.
QString compactArguments(const QJsonObject &arguments) {
    QStringList parts;
    for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it) {
        QString value;
        switch (it.value().type()) {
        case QJsonValue::String: value = it.value().toString(); break;
        case QJsonValue::Bool: value = it.value().toBool() ? u"true"_s : u"false"_s; break;
        case QJsonValue::Double: value = QString::number(it.value().toDouble()); break;
        default: value = QString::fromUtf8(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact)); break;
        }
        if (it.value().isArray()) value = QString::fromUtf8(QJsonDocument(it.value().toArray()).toJson(QJsonDocument::Compact));
        value.replace(u'\n', u' ');
        parts.append(arguments.size() == 1 ? value : u"%1=%2"_s.arg(it.key(), value));
    }
    return parts.join(u" · "_s);
}

} // namespace

// Paints one action as a card of three lines.
class ActionLogDock::CardDelegate : public QStyledItemDelegate {
public:
    explicit CardDelegate(ActionLogDock *dock) : QStyledItemDelegate(dock), dock_(dock) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QRect card = option.rect.adjusted(kPadX, kPadY, -kPadX, -kPadY);
        const QFont mono = monoFont(option.font, 0.92);
        QFont small = mono;
        small.setPointSizeF(option.font.pointSizeF() * 0.78);
        const QFontMetrics monoMetrics(mono);
        const QFontMetrics textMetrics(option.font);
        const QFontMetrics smallMetrics(small);
        const QColor text = option.palette.color(QPalette::Text);
        const bool running = index.data(RunningRole).toBool();
        const bool ok = index.data(OkRole).toBool();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        int y = card.top();

        // The tool, its source as a chip, and the time on the right.
        const QString tool = index.data(ToolRole).toString();
        painter->setFont(mono);
        painter->setPen(text);
        const QRect first(card.left(), y, card.width(), monoMetrics.height());
        painter->drawText(first, Qt::AlignLeft | Qt::AlignVCenter, tool);
        const QString source = index.data(SourceRole).toString();
        if (!source.isEmpty()) {
            const bool voice = source.contains(u"voice"_s);
            const int chipHeight = smallMetrics.height() + 4;
            const QRect chip(card.left() + monoMetrics.horizontalAdvance(tool) + 8, y + (first.height() - chipHeight) / 2,
                             smallMetrics.horizontalAdvance(source) + 14, chipHeight);
            painter->setPen(Qt::NoPen);
            painter->setBrush(voice ? dock_->voiceChipColor() : dock_->chipColor());
            painter->drawRoundedRect(chip, chipHeight / 2.0, chipHeight / 2.0);
            painter->setFont(small);
            painter->setPen(voice ? dock_->accentColor() : dock_->mutedColor());
            painter->drawText(chip, Qt::AlignCenter, source);
        }
        painter->setFont(mono);
        painter->setPen(dock_->mutedColor());
        const QString when = running ? tr("running…") : u"%1 ms"_s.arg(index.data(ElapsedRole).toLongLong());
        painter->drawText(first, Qt::AlignRight | Qt::AlignVCenter, when);
        y += first.height() + kLineGap;

        // The arguments.
        QString arguments = index.data(ArgumentsRole).toString();
        if (arguments.isEmpty()) arguments = tr("no arguments");
        painter->setPen(dock_->mutedColor());
        painter->drawText(QRect(card.left(), y, card.width(), monoMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          monoMetrics.elidedText(arguments, Qt::ElideRight, card.width()));
        y += monoMetrics.height() + kLineGap;

        // What came back.
        const QString result = running ? u"…"_s : index.data(ResultRole).toString();
        painter->setFont(option.font);
        painter->setPen(running ? dock_->mutedColor() : ok ? text : dock_->errorColor());
        painter->drawText(QRect(card.left(), y, card.width(), textMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          textMetrics.elidedText(result, Qt::ElideRight, card.width()));

        painter->setPen(QPen(dock_->lineColor(), 1));
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        const QFontMetrics monoMetrics(monoFont(option.font, 0.92));
        const QFontMetrics textMetrics(option.font);
        return {option.rect.width(), 2 * kPadY + 2 * monoMetrics.height() + textMetrics.height() + 2 * kLineGap + 1};
    }

private:
    QFont monoFont(const QFont &base, double scale) const {
        QFont mono = dock_->monoFamily().isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                                                   : QFont(dock_->monoFamily());
        mono.setPointSizeF(base.pointSizeF() * scale);
        return mono;
    }

    ActionLogDock *dock_;
};

ActionLogDock::ActionLogDock(const QString &auditPath, QWidget *parent)
    : QDockWidget(tr("Actions"), parent), list_(new QListWidget(this)) {
    setObjectName(u"actionLog"_s);
    setFeatures(QDockWidget::DockWidgetClosable);
    const ThemeTokens tokens = NexaTheme::tokens(ThemeScheme::Dark);
    muted_ = tokens.muted;
    line_ = tokens.line;
    accent_ = tokens.idle;
    error_ = tokens.error;
    chip_ = tokens.panelRaised;
    voiceChip_ = tokens.idle;
    voiceChip_.setAlphaF(0.18);

    // The title: what this is, and the file that keeps it.
    auto *title = new QWidget(this);
    title->setObjectName(u"dockTitle"_s);
    auto *titleLayout = new QHBoxLayout(title);
    titleLayout->setContentsMargins(kPadX, 12, kPadX, 12);
    auto *text = new QLabel(tr("Actions"), title);
    text->setObjectName(u"dockTitleText"_s);
    auto *note = new QLabel(QFileInfo(auditPath).fileName(), title);
    note->setObjectName(u"dockTitleNote"_s);
    note->setToolTip(tr("Every action is also written to %1").arg(auditPath));
    titleLayout->addWidget(text);
    titleLayout->addStretch();
    titleLayout->addWidget(note);
    setTitleBarWidget(title);

    list_->setObjectName(u"actionList"_s);
    list_->setItemDelegate(new CardDelegate(this));
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setUniformItemSizes(true);
    list_->setMouseTracking(true);

    auto *clear = new QAction(tr("Clear"), list_);
    connect(clear, &QAction::triggered, this, [this] {
        list_->clear();
        items_.clear();
    });
    list_->setContextMenuPolicy(Qt::ActionsContextMenu);
    list_->addAction(clear);
    setWidget(list_);
}

void ActionLogDock::actionStarted(quint64 id, const QString &tool, const QJsonObject &arguments, const QString &source) {
    auto *item = new QListWidgetItem;
    item->setData(IdRole, id);
    item->setData(ToolRole, tool);
    item->setData(SourceRole, source);
    item->setData(ArgumentsRole, compactArguments(arguments));
    item->setData(RunningRole, true);
    item->setData(OkRole, true);
    item->setData(ElapsedRole, qint64(0));
    const QString json = arguments.isEmpty() ? tr("(no arguments)")
                                             : QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented));
    item->setToolTip(u"%1 · %2\n%3"_s.arg(tool, source, json));
    list_->insertItem(0, item);
    items_.insert(id, item);
    trim();
}

void ActionLogDock::actionFinished(quint64 id, bool ok, const QString &summary, qint64 elapsedMs) {
    QListWidgetItem *item = items_.take(id);
    if (!item) return;
    item->setData(RunningRole, false);
    item->setData(OkRole, ok);
    item->setData(ResultRole, summary);
    item->setData(ElapsedRole, elapsedMs);
    item->setToolTip(item->toolTip() + u"\n\n"_s + (ok ? u"✓ "_s : u"✗ "_s) + summary + u" · %1 ms"_s.arg(elapsedMs));
}

void ActionLogDock::trim() {
    while (list_->count() > kMaxRows) {
        QListWidgetItem *oldest = list_->takeItem(list_->count() - 1);
        items_.remove(oldest->data(IdRole).toULongLong());
        delete oldest;
    }
}

void ActionLogDock::repaint() {
    list_->viewport()->update();
}

} // namespace nexa
