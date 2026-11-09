#pragma once

#include "app/MissionPanel.h"

class QLabel;
class QListView;

namespace nexa {

class MissionControl;
class SparklineWidget;
class TaskListModel;

// Mission Control's left rail: "AGENTS · 5 active · 4 / 6 slots", the voice's own card (what it listens for, a level
// meter), one card per agent at work — its id and model, its state, its task, a sparkline of its activity and how long
// it has run — and "+ Spawn agent", which adds tasks.
class AgentsRail : public MissionPanel {
    Q_OBJECT
public:
    AgentsRail(TaskListModel *tasks, QWidget *parent = nullptr);

    // The voice card: its state chip ("LISTENING", "WAKE WORD ON", "OFF"), its line, the engines it hears and speaks with.
    void setVoice(const QString &chip, const QString &line, const QString &engines);
    void pushVoiceLevel(int percent);

signals:
    void taskActivated(quint64 id);
    void spawnRequested();

private:
    void refreshCount();

    MissionControl *mission_;
    QLabel *count_;
    QListView *list_;
    QLabel *voiceChip_;
    QLabel *voiceLine_;
    QLabel *voiceEngines_;
    SparklineWidget *voiceLevel_;
};

} // namespace nexa
