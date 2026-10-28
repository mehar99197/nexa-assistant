#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

namespace nexa {

// Line icons painted with QPainter in whatever colour the theme asks for, so they read on dark
// and light alike (theme icons come in one colour and would vanish on the other scheme). The
// four node icons are NexaCorePainter's, so a button means the same as the satellite it points at.
class NexaIcons {
public:
    enum class Kind { Mic, Pc, Tools, Phone, Send, Stop, Sun, Moon, Gear, Refresh, NewChat, Actions, More, Eye };

    static QIcon icon(Kind kind, const QColor &colour, int size = 20);
    static void paint(QPainter &painter, Kind kind, const QRectF &box, const QColor &colour);
};

} // namespace nexa
