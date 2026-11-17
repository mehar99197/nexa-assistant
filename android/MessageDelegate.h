#pragma once

#include "ui/NexaDesigns.h"
#include "ui/NexaTheme.h"

#include <QFont>
#include <QRect>
#include <QStyledItemDelegate>

class QAbstractItemView;

namespace nexa {

// Paints the conversation's lines as the design draws them: what you said in a bubble on the
// right (what the PC typed in a dimmer one), Nexa's words plain on the left, steps and transcripts
// small in monospace, errors in the error colour. The item's text stays the plain message, so
// copying and the tests read it unchanged. Colours come from the theme's tokens (setTokens).
class MessageDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    enum { KindRole = Qt::UserRole + 1 };       // ChatPage::Kind, as an int

    explicit MessageDelegate(QAbstractItemView *view);

    void setTokens(const ThemeTokens &tokens);
    void setDesign(BubbleStyle bubbles, int radius, FontFace face);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    struct Shape {
        QFont font;
        QRect bubble;       // empty for lines without one
        QRect text;
        int height{0};
    };
    Shape shape(const QStyleOptionViewItem &option, const QModelIndex &index) const;

    QAbstractItemView *view_;
    ThemeTokens tokens_;
    BubbleStyle bubbles_{BubbleStyle::Bubbles};
    int radius_{14};
    FontFace face_{FontFace::Sans};
};

} // namespace nexa
