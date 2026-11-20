#pragma once

#include "core/Result.h"

#include <QHash>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

namespace nexa::uihierarchy {

// One view of the phone's screen as `uiautomator dump` describes it.
struct UiNode {
    QString className;      // "android.widget.Button"
    QString text;
    QString description;    // content-desc
    QString resourceId;     // "com.app:id/search_button"
    QString package;
    QRect bounds;           // the phone's own pixels
    int depth{0};
    bool clickable{false};
    bool longClickable{false};
    bool checkable{false};
    bool checked{false};
    bool enabled{true};
    bool focused{false};
    bool scrollable{false};
    bool selected{false};
    bool password{false};
};

// The dump's XML (whatever uiautomator printed before it is skipped) → the views in document order. Parse when there
// is no hierarchy in it, with uiautomator's own line ("ERROR: could not get idle state.") when that is what came.
Result<QList<UiNode>> parse(const QString &xml);

// The phone's screen as text, the way Playwright writes a page: one line per element that says or does something,
// a ref (p1, p2, …) on each; bare layouts are lifted out so their children stand in their place.
struct PhoneOutline {
    QString text;
    QHash<QString, QRect> refs;         // "p7" → its bounds (the tap goes to the centre)
    QHash<QString, QString> names;      // "p7" → its words, for the tools' sentences
    QString package;                    // the app in front
    bool truncated{false};
};

PhoneOutline outline(const QList<UiNode> &nodes, const QString &filter = {}, int maxLines = 300, int maxChars = 12000);

// Where a tap on an element lands: the middle of its bounds ([924,84][1032,192] → 978, 138).
QPoint centre(const QRect &bounds);

// A view's class as a person says it: "Button", "ImageButton" → button; "EditText" → entry; "TextView" → text;
// "RecyclerView", "ListView", "ScrollView" → list; anything else → item.
QString role(const QString &className);

} // namespace nexa::uihierarchy
