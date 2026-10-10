#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace nexa {

// The first screen: where Nexa is and the six-digit code Settings → Remote shows on the PC. The
// address field also takes the whole link from the PC's QR code, which carries the code with it.
class PairPage : public QWidget {
    Q_OBJECT
public:
    explicit PairPage(QWidget *parent = nullptr);

    void setAddress(const QString &address);
    void setDeviceName(const QString &name);
    QString deviceName() const;
    void setBusy(bool busy);
    void setError(const QString &text);

signals:
    void pairRequested(const QString &address, const QString &code, const QString &name);

private:
    void submit();

    QLineEdit *address_{nullptr};
    QLineEdit *code_{nullptr};
    QLineEdit *name_{nullptr};
    QPushButton *pair_{nullptr};
    QLabel *error_{nullptr};
};

} // namespace nexa
