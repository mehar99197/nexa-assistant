#include "app/VoiceEnrolmentDialog.h"

#include "voice/listen/SpeakerGate.h"
#include "voice/pipeline/VoicePipeline.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRecordMs = 4'500;

} // namespace

VoiceEnrolmentDialog::VoiceEnrolmentDialog(VoicePipeline *voice, SpeakerGate *gate, const QString &wakeName, QWidget *parent)
    : QDialog(parent), voice_(voice), gate_(gate) {
    setWindowTitle(tr("Enrol my voice"));
    setObjectName(u"voiceEnrolment"_s);
    resize(520, 300);
    sentences_ = {tr("Hey %1, what is the weather like today?").arg(wakeName),
                  tr("%1, open the calculator and set the volume to fifty.").arg(wakeName),
                  tr("Read me my unread messages, and then remind me about the meeting at five.")};
    auto *column = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Read each sentence aloud, at your usual distance from the microphone, after pressing "
                                "Record. Nexa keeps only a short list of numbers that describes your voice — never the "
                                "recordings — and from then on answers hands-free to your voice alone."),
                             this);
    intro->setWordWrap(true);
    prompt_ = new QLabel(this);
    prompt_->setObjectName(u"enrolPrompt"_s);
    prompt_->setWordWrap(true);
    QFont large = prompt_->font();
    large.setPointSizeF(large.pointSizeF() * 1.3);
    large.setBold(true);
    prompt_->setFont(large);
    status_ = new QLabel(this);
    status_->setObjectName(u"enrolStatus"_s);
    status_->setWordWrap(true);
    record_ = new QPushButton(tr("Record"), this);
    record_->setObjectName(u"enrolRecord"_s);
    check_ = new QPushButton(tr("Check it is me"), this);
    check_->setObjectName(u"enrolCheck"_s);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    save_ = buttons->addButton(tr("Save my voice"), QDialogButtonBox::AcceptRole);
    save_->setObjectName(u"enrolSave"_s);
    connect(record_, &QPushButton::clicked, this, &VoiceEnrolmentDialog::record);
    connect(check_, &QPushButton::clicked, this, &VoiceEnrolmentDialog::check);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(intro);
    column->addSpacing(8);
    column->addWidget(prompt_);
    column->addWidget(status_);
    auto *row = new QHBoxLayout;
    row->addWidget(record_);
    row->addWidget(check_);
    row->addStretch();
    column->addLayout(row);
    column->addStretch();
    column->addWidget(buttons);
    refresh();
}

void VoiceEnrolmentDialog::record() {
    if (busy_ || embeddings_.size() >= sentences_.size()) return;
    busy_ = true;
    status_->setText(tr("Recording — read the sentence now…"));
    refresh();
    voice_->recordFor(kRecordMs, [this](const QByteArray &pcm) { recorded(pcm); });
}

void VoiceEnrolmentDialog::recorded(const QByteArray &pcm) {
    status_->setText(tr("Listening to it…"));
    gate_->embed(pcm, [this](std::optional<QList<float>> embedding) {
        busy_ = false;
        if (!embedding) {
            status_->setText(tr("That did not work — was the microphone silent? Press Record and read it again."));
            refresh();
            return;
        }
        embeddings_.append(*embedding);
        if (embeddings_.size() == sentences_.size()) {
            profile_ = VoiceProfile::fromRecordings(gate_->modelName(), embeddings_);
            status_->setText(tr("Done. Check it is you with one more sentence, or save it now."));
        } else {
            status_->setText(tr("Got it (%1 of %2).").arg(embeddings_.size()).arg(sentences_.size()));
        }
        refresh();
    });
}

void VoiceEnrolmentDialog::check() {
    if (busy_ || !profile_) return;
    busy_ = true;
    prompt_->setText(tr("Say anything you like."));
    status_->setText(tr("Recording…"));
    refresh();
    voice_->recordFor(kRecordMs, [this](const QByteArray &pcm) {
        gate_->embed(pcm, [this](std::optional<QList<float>> embedding) {
            busy_ = false;
            if (!embedding) {
                status_->setText(tr("Nothing to compare — try again."));
            } else {
                const double score = profile_->score(*embedding);
                status_->setText(score >= gate_->threshold()
                                     ? tr("That is you (a match of %1). Save it.").arg(score, 0, 'f', 2)
                                     : tr("That was not clearly you (a match of %1). Record the sentences again in a "
                                          "quieter moment, or lower \"How sure it is me\".")
                                           .arg(score, 0, 'f', 2));
            }
            refresh();
        });
    });
}

void VoiceEnrolmentDialog::refresh() {
    const qsizetype done = embeddings_.size();
    if (done < sentences_.size() && !busy_) {
        prompt_->setText(u"“%1”"_s.arg(sentences_.at(done)));
    }
    record_->setEnabled(!busy_ && done < sentences_.size());
    record_->setText(done == 0 ? tr("Record") : tr("Record the next"));
    check_->setEnabled(!busy_ && profile_.has_value());
    save_->setEnabled(!busy_ && profile_.has_value());
}

} // namespace nexa
