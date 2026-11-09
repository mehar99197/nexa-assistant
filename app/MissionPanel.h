#pragma once

#include "core/mission/AgentRole.h"
#include "core/mission/Task.h"

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QString>

class QPainter;

namespace nexa {

// The colours every part of Mission Control draws with, set from theme.qss (`nexa--MissionPanel { qproperty-… }`
// reaches every subclass) and defaulting to the Mission palette's tokens: a colour per lane of work and per state, so
// an agent's id, its progress bar and its activity lines match. Never colour alone: every chip also says its state.
class MissionPanel : public QFrame {
    Q_OBJECT
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor)
    Q_PROPERTY(QColor mutedColor READ mutedColor WRITE setMutedColor)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor)
    Q_PROPERTY(QColor raisedColor READ raisedColor WRITE setRaisedColor)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
    Q_PROPERTY(QColor goodColor READ goodColor WRITE setGoodColor)
    Q_PROPERTY(QColor warnColor READ warnColor WRITE setWarnColor)
    Q_PROPERTY(QColor errorColor READ errorColor WRITE setErrorColor)
    Q_PROPERTY(QColor webColor READ webColor WRITE setWebColor)
    Q_PROPERTY(QColor codeColor READ codeColor WRITE setCodeColor)
    Q_PROPERTY(QColor screenColor READ screenColor WRITE setScreenColor)
    Q_PROPERTY(QColor phoneColor READ phoneColor WRITE setPhoneColor)
    Q_PROPERTY(QColor voiceColor READ voiceColor WRITE setVoiceColor)
    Q_PROPERTY(QString monoFamily READ monoFamily WRITE setMonoFamily)
public:
    explicit MissionPanel(QWidget *parent = nullptr);

    QColor laneColor(Lane lane) const;
    QColor roleColor(AgentRole role) const { return laneColor(laneFor(role)); }
    QColor stateColor(TaskState state) const;
    QFont mono(double points = 9.0, bool bold = false) const;
    // A small chip: the words in the colour, on a faint fill of it, with a hairline of it.
    void paintChip(QPainter &painter, const QRectF &box, const QString &text, const QColor &colour) const;
    static QString elapsed(const Task &task);       // "04:32", "1:02:07"

    QColor textColor() const { return text_; }
    QColor mutedColor() const { return muted_; }
    QColor lineColor() const { return line_; }
    QColor raisedColor() const { return raised_; }
    QColor accentColor() const { return accent_; }
    QColor goodColor() const { return good_; }
    QColor warnColor() const { return warn_; }
    QColor errorColor() const { return error_; }
    QColor webColor() const { return web_; }
    QColor codeColor() const { return code_; }
    QColor screenColor() const { return screen_; }
    QColor phoneColor() const { return phone_; }
    QColor voiceColor() const { return voice_; }
    QString monoFamily() const { return mono_; }
    void setTextColor(const QColor &c) { set(text_, c); }
    void setMutedColor(const QColor &c) { set(muted_, c); }
    void setLineColor(const QColor &c) { set(line_, c); }
    void setRaisedColor(const QColor &c) { set(raised_, c); }
    void setAccentColor(const QColor &c) { set(accent_, c); }
    void setGoodColor(const QColor &c) { set(good_, c); }
    void setWarnColor(const QColor &c) { set(warn_, c); }
    void setErrorColor(const QColor &c) { set(error_, c); }
    void setWebColor(const QColor &c) { set(web_, c); }
    void setCodeColor(const QColor &c) { set(code_, c); }
    void setScreenColor(const QColor &c) { set(screen_, c); }
    void setPhoneColor(const QColor &c) { set(phone_, c); }
    void setVoiceColor(const QColor &c) { set(voice_, c); }
    void setMonoFamily(const QString &family) {
        mono_ = family;
        update();
    }

signals:
    void coloursChanged();

private:
    void set(QColor &member, const QColor &colour);

    QColor text_{0xE7, 0xE9, 0xF2};
    QColor muted_{0x8A, 0x90, 0xA6};
    QColor line_{0x23, 0x27, 0x36};
    QColor raised_{0x19, 0x1C, 0x25};
    QColor accent_{0xA8, 0x55, 0xF7};
    QColor good_{0x22, 0xC5, 0x5E};
    QColor warn_{0xF5, 0x9E, 0x0B};
    QColor error_{0xF8, 0x71, 0x71};
    QColor web_{0x38, 0xBD, 0xF8};
    QColor code_{0x81, 0x8C, 0xF8};
    QColor screen_{0x22, 0xC5, 0x5E};
    QColor phone_{0xC0, 0x84, 0xFC};
    QColor voice_{0xE8, 0x79, 0xF9};
    QString mono_{QStringLiteral("monospace")};
};

} // namespace nexa
