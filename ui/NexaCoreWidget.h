#pragma once

#include "ui/NexaCorePainter.h"
#include "ui/NexaCoreSpec.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <array>
#include <optional>

namespace nexa {

// The Nexa core: the state light that is also the brand. It owns no logic — it is told a mode, the
// microphone level and which satellites to light — and paints NexaCoreSpec's motion with
// NexaCorePainter: 60 Hz while something happens, 30 Hz for idle breathing, nothing while hidden.
// A state change cross-fades colour and opacity over kCrossFadeMs so nothing pops, and the
// fast-path snap stays for at least its ping. The colours are Q_PROPERTYs set from theme.qss, so
// the stylesheet stays the single source. One widget, three sizes: the presence beside the chat
// (nodes on), the Talk button (NexaCoreButton), and a snapshot() for the tray.
class NexaCoreWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QColor idleColor READ idleColor WRITE setIdleColor)
    Q_PROPERTY(QColor wakeColor READ wakeColor WRITE setWakeColor)
    Q_PROPERTY(QColor listenColor READ listenColor WRITE setListenColor)
    Q_PROPERTY(QColor sttColor READ sttColor WRITE setSttColor)
    Q_PROPERTY(QColor thinkColor READ thinkColor WRITE setThinkColor)
    Q_PROPERTY(QColor actColor READ actColor WRITE setActColor)
    Q_PROPERTY(QColor speakColor READ speakColor WRITE setSpeakColor)
    Q_PROPERTY(QColor fastColor READ fastColor WRITE setFastColor)
    Q_PROPERTY(QColor errorColor READ errorColor WRITE setErrorColor)
    Q_PROPERTY(QColor panelColor READ panelColor WRITE setPanelColor)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor)
    Q_PROPERTY(QColor mutedColor READ mutedColor WRITE setMutedColor)
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor)
    Q_PROPERTY(QColor groundColor READ groundColor WRITE setGroundColor)
public:
    explicit NexaCoreWidget(QWidget *parent = nullptr);

    CoreMode mode() const { return mode_; }
    void setMode(CoreMode mode);
    void setLevel(double level);                // the microphone, 0 … 1
    void setLitNodes(CoreNodes nodes);          // satellites an action lights, besides the state's own
    void setShowNodes(bool show);
    void setShowLabels(bool show);
    void setDiameter(int pixels);               // a fixed size; 0 follows the layout
    void setAnimated(bool animated);            // false: one still frame (snapshots, tests)
    bool isAnimated() const { return animated_; }

    QColor colorFor(CoreMode mode) const;
    // A still picture of a state (its motion at rest), for the tray icon and the window icon.
    QPixmap snapshot(CoreMode mode, int size, bool nodes = false, qreal devicePixelRatio = 1.0) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    QColor idleColor() const { return colors_[0]; }
    QColor wakeColor() const { return colors_[1]; }
    QColor listenColor() const { return colors_[2]; }
    QColor sttColor() const { return colors_[3]; }
    QColor thinkColor() const { return colors_[4]; }
    QColor actColor() const { return colors_[5]; }
    QColor speakColor() const { return colors_[6]; }
    QColor fastColor() const { return colors_[7]; }
    QColor errorColor() const { return colors_[8]; }
    QColor panelColor() const { return panel_; }
    QColor lineColor() const { return line_; }
    QColor mutedColor() const { return muted_; }
    QColor textColor() const { return text_; }
    QColor groundColor() const { return ground_; }
    void setIdleColor(const QColor &c) { setColor(0, c); }
    void setWakeColor(const QColor &c) { setColor(1, c); }
    void setListenColor(const QColor &c) { setColor(2, c); }
    void setSttColor(const QColor &c) { setColor(3, c); }
    void setThinkColor(const QColor &c) { setColor(4, c); }
    void setActColor(const QColor &c) { setColor(5, c); }
    void setSpeakColor(const QColor &c) { setColor(6, c); }
    void setFastColor(const QColor &c) { setColor(7, c); }
    void setErrorColor(const QColor &c) { setColor(8, c); }
    void setPanelColor(const QColor &c) { panel_ = c; update(); }
    void setLineColor(const QColor &c) { line_ = c; update(); }
    void setMutedColor(const QColor &c) { muted_ = c; update(); }
    void setTextColor(const QColor &c) { text_ = c; update(); }
    void setGroundColor(const QColor &c) { ground_ = c; update(); }

signals:
    void modeChanged(nexa::CoreMode mode);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void setColor(int index, const QColor &colour);
    void applyMode(CoreMode mode);
    void tick();
    void restartTimer();
    double seconds() const { return clock_.elapsed() / 1000.0; }
    CoreNodes litFor(CoreMode mode) const;
    void paintFrame(QPainter &painter, const QRectF &rect) const;

    std::array<QColor, kCoreModeCount> colors_;
    QColor panel_;
    QColor line_;
    QColor muted_;
    QColor text_;
    QColor ground_;
    CoreMode mode_{CoreMode::Idle};
    CoreMode previous_{CoreMode::Idle};
    std::optional<CoreMode> pending_;       // waits until the fast-path snap has shown
    QElapsedTimer clock_;
    qint64 modeStartMs_{0};
    qint64 fadeStartMs_{-1};                // -1: no cross-fade running
    double level_{0.0};
    double shownLevel_{0.0};                // follows level_ smoothly, so the ticks swell rather than jump
    CoreNodes lit_{NoNode};
    bool nodes_{true};
    bool labels_{false};
    bool animated_{true};
    int diameter_{0};
    QTimer timer_;
};

// The core as a button: the Talk button. Press and release are reported as they happen, so the
// owner can tell a tap from a hold; Space does the same from the keyboard.
class NexaCoreButton : public NexaCoreWidget {
    Q_OBJECT
public:
    explicit NexaCoreButton(QWidget *parent = nullptr);

signals:
    void pressed();
    void released();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    bool held_{false};
};

} // namespace nexa
