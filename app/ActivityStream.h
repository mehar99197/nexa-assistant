#pragma once

#include "app/MissionPanel.h"
#include "core/mission/ActivityFeed.h"

#include <QAbstractListModel>
#include <QHash>

#include <optional>

class QButtonGroup;
class QLabel;
class QListView;
class QVBoxLayout;

namespace nexa {

class MissionControl;

// The live activity as a list (Adapter over the board's ActivityFeed), newest last, one lane or all of them.
class ActivityModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { LaneRole = Qt::UserRole + 1, TimeRole, OkRole };

    explicit ActivityModel(MissionControl *mission, QObject *parent = nullptr);
    void setLane(std::optional<Lane> lane);
    std::optional<Lane> lane() const { return lane_; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
    MissionControl *mission_;
    std::optional<Lane> lane_;
    QList<ActivityEvent> rows_;
};

// Mission Control's right column: "LIVE ACTIVITY · STREAMING", the lane chips (ALL, WEB, CODE, SCREEN, PHONE, VOICE),
// a card for each question a task waits on — who asks, what for, Approve and Deny — and the lines of what happens:
// "14:37:48  WEB  browser_click → Submit".
class ActivityStream : public MissionPanel {
    Q_OBJECT
public:
    explicit ActivityStream(MissionControl *mission, QWidget *parent = nullptr);

signals:
    void approved(quint64 actionId);
    void denied(quint64 actionId);

private:
    void addApproval(quint64 task, quint64 actionId);
    void dropApproval(quint64 actionId);

    MissionControl *mission_;
    ActivityModel *model_;
    QListView *lines_;
    QVBoxLayout *approvals_;
    QHash<quint64, QWidget *> cards_;
    QButtonGroup *chips_;
};

} // namespace nexa
