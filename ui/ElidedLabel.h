#pragma once

#include <QLabel>
#include <QString>

namespace nexa {

// A one-line label that fits whatever width it is given: its words are cut with "…" instead of widening the layout
// (a status pill on a phone 360 pixels wide must not push the buttons beside it off the screen). The full text is the
// tooltip while it is cut. An optional rich-text prefix — a coloured dot — is never cut.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget *parent = nullptr);

    // `prefixHtml` is shown as it is, before the text (which is plain, and escaped here).
    void setElidedText(const QString &text, const QString &prefixHtml = {});
    QString fullText() const { return text_; }
    bool isElided() const { return elided_; }

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void refit();
    int prefixWidth() const;

    QString text_;
    QString prefix_;
    bool elided_{false};
};

} // namespace nexa
