#pragma once

#include <QColor>
#include <QDockWidget>
#include <QImage>
#include <QString>

class QLabel;
class QToolButton;

namespace nexa {

class AndroidDevice;
class PhoneMirror;

// The phone beside the chat, whenever one is connected: which phone it is, and its screen live —
// a mirror through adb (PhoneMirror), one picture after another while the panel is in view, paused
// with a click. The window shows the panel when a phone comes and hides it when it goes
// (phoneChanged); Ctrl+P shows it by hand. Colours are Q_PROPERTYs set from theme.qss.
class PhonePanel : public QDockWidget {
    Q_OBJECT
    Q_PROPERTY(QColor mutedColor READ mutedColor WRITE setMutedColor)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
    Q_PROPERTY(QColor errorColor READ errorColor WRITE setErrorColor)
public:
    explicit PhonePanel(AndroidDevice *phone, QWidget *parent = nullptr);

    bool isConnected() const { return connected_; }
    bool isPaused() const { return paused_; }
    void setPaused(bool paused);
    // A picture of the phone's screen to show (the mirror's, or one handed in to look at the panel).
    void showFrame(const QImage &frame, qint64 elapsedMs);

    QColor mutedColor() const { return muted_; }
    QColor lineColor() const { return line_; }
    QColor accentColor() const { return accent_; }
    QColor errorColor() const { return error_; }
    void setMutedColor(const QColor &c) { muted_ = c; update(); }
    void setLineColor(const QColor &c) { line_ = c; update(); }
    void setAccentColor(const QColor &c) { accent_ = c; update(); }
    void setErrorColor(const QColor &c) { error_ = c; update(); }

signals:
    void phoneChanged(bool connected);

private:
    class ScreenView;
    void showDevice(const QString &description);
    void refreshInfo();
    void updateMirror();        // runs while in view, connected and not paused
    void showStatus(const QString &text, bool isError);

    AndroidDevice *phone_;
    PhoneMirror *mirror_;
    QLabel *note_;              // in the title: which phone
    QLabel *info_;              // model, Android version, battery
    QLabel *status_;            // "Live · 0.8 s per frame", "Paused", or what went wrong
    QToolButton *pause_;
    ScreenView *screen_;
    bool connected_{false};
    bool paused_{false};
    QColor muted_;
    QColor line_;
    QColor accent_;
    QColor error_;
};

} // namespace nexa
