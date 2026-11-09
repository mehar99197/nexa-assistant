#include "app/MissionSettingsPage.h"

#include "core/mission/AgentRole.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString whatItDoes(AgentRole role) {
    switch (role) {
    case AgentRole::Web: return QObject::tr("sites, forms, mail — in the browser");
    case AgentRole::Code: return QObject::tr("programs, scripts, tests (Auto: Codex, opencode or agy)");
    case AgentRole::Screen: return QObject::tr("what is on the PC's screen (Auto: a provider that sees)");
    case AgentRole::Phone: return QObject::tr("the Android phone (Auto: a provider that sees)");
    case AgentRole::General: return QObject::tr("everything else");
    }
    return {};
}

} // namespace

MissionSettingsPage::MissionSettingsPage(const Config &config, QWidget *parent) : QWidget(parent) {
    auto *column = new QVBoxLayout(this);
    auto *slotsRow = new QHBoxLayout;
    slots_ = new QSpinBox(this);
    slots_->setObjectName(u"maxConcurrent"_s);
    slots_->setRange(1, MissionConfig::kMaxSlots);
    slots_->setValue(config.mission.maxConcurrent);
    slotsRow->addWidget(new QLabel(tr("Run up to"), this));
    slotsRow->addWidget(slots_);
    slotsRow->addWidget(new QLabel(tr("tasks at once; the rest wait in the queue."), this));
    slotsRow->addStretch();
    column->addLayout(slotsRow);
    auto *note = new QLabel(tr("A program (agy, opencode, Codex) answers one conversation at a time, so each task on one "
                               "runs its own copy of it: more slots mean more of them at once."),
                            this);
    note->setWordWrap(true);
    column->addWidget(note);

    auto *box = new QGroupBox(tr("The model each kind of agent works with"), this);
    auto *form = new QFormLayout(box);
    for (const AgentRole role : kAgentRoles) {
        auto *row = new QHBoxLayout;
        auto *provider = new QComboBox(box);
        provider->setObjectName(u"roleProvider_%1"_s.arg(roleKey(role)));
        provider->addItem(tr("Auto"), QString());
        for (const ProviderConfig &configured : config.providers) provider->addItem(configured.name, configured.id);
        auto *model = new QLineEdit(box);
        model->setObjectName(u"roleModel_%1"_s.arg(roleKey(role)));
        model->setPlaceholderText(tr("the provider's own default"));
        const ModelChoice chosen = config.mission.roleModels.value(roleKey(role));
        if (const int at = provider->findData(chosen.provider); !chosen.isAuto() && at >= 0) {
            provider->setCurrentIndex(at);
            model->setText(chosen.model);
        }
        model->setEnabled(provider->currentIndex() > 0);
        connect(provider, &QComboBox::currentIndexChanged, model, [model](int index) { model->setEnabled(index > 0); });
        row->addWidget(provider, 1);
        row->addWidget(model, 1);
        auto *label = new QLabel(u"%1 <span style=\"opacity:0.7\">— %2</span>"_s.arg(roleName(role), whatItDoes(role)), box);
        label->setTextFormat(Qt::RichText);
        form->addRow(label);
        form->addRow(row);
    }
    column->addWidget(box);
    column->addStretch();
}

void MissionSettingsPage::store(MissionConfig &mission) const {
    mission.maxConcurrent = slots_->value();
    mission.roleModels.clear();
    for (const AgentRole role : kAgentRoles) {
        const auto *provider = findChild<QComboBox *>(u"roleProvider_%1"_s.arg(roleKey(role)));
        const auto *model = findChild<QLineEdit *>(u"roleModel_%1"_s.arg(roleKey(role)));
        if (!provider || !model || provider->currentData().toString().isEmpty()) continue;
        mission.roleModels.insert(roleKey(role), ModelChoice{provider->currentData().toString(), model->text().trimmed()});
    }
}

} // namespace nexa
