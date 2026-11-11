#include "app/VoiceSampleDialog.h"

#include "voice/pipeline/VoicePipeline.h"
#include "voice/tts/PcmPlayer.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

VoiceSampleDialog::VoiceSampleDialog(VoicePipeline *voice, QWidget *parent)
    : QDialog(parent), voice_(voice), player_(std::make_unique<PcmPlayer>()) {
    setWindowTitle(tr("Record my voice"));
    setObjectName(u"voiceSample"_s);
    resize(560, 340);
    auto *column = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Press Record and read the passage below aloud, at your usual pace and distance from the "
                                "microphone; it listens for %1 seconds. Nexa keeps this one recording on this computer "
                                "(readable by you alone), never sends it anywhere, and shapes its voice from it. "
                                "\"Forget the sample\" in Settings removes it.")
                                .arg(VoiceSample::kRecordMs / 1000),
                             this);
    intro->setWordWrap(true);
    passage_ = new QLabel(u"“%1”"_s.arg(VoiceSample::passage()), this);
    passage_->setObjectName(u"samplePassage"_s);
    passage_->setWordWrap(true);
    passage_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont large = passage_->font();
    large.setPointSizeF(large.pointSizeF() * 1.25);
    large.setBold(true);
    passage_->setFont(large);
    status_ = new QLabel(this);
    status_->setObjectName(u"sampleStatus"_s);
    status_->setWordWrap(true);
    record_ = new QPushButton(tr("Record"), this);
    record_->setObjectName(u"sampleRecord"_s);
    play_ = new QPushButton(tr("Play it back"), this);
    play_->setObjectName(u"samplePlay"_s);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    save_ = buttons->addButton(tr("Use this voice"), QDialogButtonBox::AcceptRole);
    save_->setObjectName(u"sampleSave"_s);
    connect(record_, &QPushButton::clicked, this, &VoiceSampleDialog::record);
    connect(play_, &QPushButton::clicked, this, &VoiceSampleDialog::play);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(player_.get(), &PcmPlayer::finished, this, [this] { refresh(); });
    column->addWidget(intro);
    column->addSpacing(8);
    column->addWidget(passage_);
    column->addWidget(status_);
    auto *row = new QHBoxLayout;
    row->addWidget(record_);
    row->addWidget(play_);
    row->addStretch();
    column->addLayout(row);
    column->addStretch();
    column->addWidget(buttons);
    refresh();
}

VoiceSampleDialog::~VoiceSampleDialog() {
    player_->stop();
}

void VoiceSampleDialog::record() {
    if (busy_ || !voice_) return;
    busy_ = true;
    player_->stop();
    status_->setText(tr("Recording — read the passage now…"));
    refresh();
    voice_->recordFor(VoiceSample::kRecordMs, [this](const QByteArray &pcm) { recorded(pcm); });
}

void VoiceSampleDialog::recorded(const QByteArray &pcm) {
    busy_ = false;
    const PcmAudio trimmed = VoiceSample::trimmed(PcmAudio{pcm, 16'000, 1});
    if (const Result<void> usable = VoiceSample::usable(trimmed); !usable) {
        status_->setText(usable.error().message);
        refresh();
        return;
    }
    VoiceSample sample;
    sample.audio = trimmed;
    sample.text = VoiceSample::passage();
    sample.recorded = QDateTime::currentDateTime();
    sample_ = sample;
    status_->setText(tr("Got %1 seconds of your voice. Play it back to check it, record again, or use it.")
                         .arg(trimmed.durationMs() / 1000.0, 0, 'f', 1));
    refresh();
}

void VoiceSampleDialog::play() {
    if (!sample_ || busy_) return;
    player_->stop();
    player_->enqueue(sample_->audio);
    refresh();
}

void VoiceSampleDialog::refresh() {
    record_->setEnabled(!busy_ && voice_);
    record_->setText(sample_ ? tr("Record again") : tr("Record"));
    play_->setEnabled(!busy_ && sample_.has_value() && !player_->isPlaying());
    save_->setEnabled(!busy_ && sample_.has_value());
    if (!voice_) status_->setText(tr("The microphone is not available in this copy of Nexa."));
}

} // namespace nexa
