#pragma once

#include <QColor>
#include <QDockWidget>
#include <QHash>
#include <QJsonObject>
#include <QString>

class QListWidget;
class QListWidgetItem;

namespace nexa {

// Live list of every action (tool call), newest first, as cards: the tool and where the request
// came from, its arguments, what came back and how long it took. Fed by ToolExecutor's signals, so
// fast-path and agent actions look the same — and the same lines go to audit.jsonl, which the
// title says. Colours are Q_PROPERTYs set from theme.qss; a delegate paints the cards.
class ActionLogDock : public QDockWidget {
    Q_OBJECT
    Q_PROPERTY(QColor mutedColor READ mutedColor WRITE setMutedColor)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
    Q_PROPERTY(QColor errorColor READ errorColor WRITE setErrorColor)
    Q_PROPERTY(QColor chipColor READ chipColor WRITE setChipColor)
    Q_PROPERTY(QColor voiceChipColor READ voiceChipColor WRITE setVoiceChipColor)
    Q_PROPERTY(QString monoFamily READ monoFamily WRITE setMonoFamily)
public:
    explicit ActionLogDock(const QString &auditPath, QWidget *parent = nullptr);

    void actionStarted(quint64 id, const QString &tool, const QJsonObject &arguments, const QString &source);
    void actionFinished(quint64 id, bool ok, const QString &summary, qint64 elapsedMs);

    QColor mutedColor() const { return muted_; }
    QColor lineColor() const { return line_; }
    QColor accentColor() const { return accent_; }
    QColor errorColor() const { return error_; }
    QColor chipColor() const { return chip_; }
    QColor voiceChipColor() const { return voiceChip_; }
    QString monoFamily() const { return mono_; }
    void setMutedColor(const QColor &c) { muted_ = c; repaint(); }
    void setLineColor(const QColor &c) { line_ = c; repaint(); }
    void setAccentColor(const QColor &c) { accent_ = c; repaint(); }
    void setErrorColor(const QColor &c) { error_ = c; repaint(); }
    void setChipColor(const QColor &c) { chip_ = c; repaint(); }
    void setVoiceChipColor(const QColor &c) { voiceChip_ = c; repaint(); }
    void setMonoFamily(const QString &family) { mono_ = family; repaint(); }

private:
    class CardDelegate;
    void trim();
    void repaint();

    QListWidget *list_;
    QHash<quint64, QListWidgetItem *> items_;
    QColor muted_;
    QColor line_;
    QColor accent_;
    QColor error_;
    QColor chip_;
    QColor voiceChip_;
    QString mono_;
};

} // namespace nexa
