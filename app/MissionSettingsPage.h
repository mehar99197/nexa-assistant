#pragma once

#include "core/config/Config.h"

#include <QWidget>

class QSpinBox;

namespace nexa {

// Settings → Mission: how many tasks run at once (the rest wait in the queue), and which provider and model each kind
// of agent works with — Auto by default, which gives code to a coding program and the screen and the phone to a
// provider that sees.
class MissionSettingsPage : public QWidget {
    Q_OBJECT
public:
    MissionSettingsPage(const Config &config, QWidget *parent = nullptr);

    void store(MissionConfig &mission) const;

private:
    QSpinBox *slots_;
};

} // namespace nexa
