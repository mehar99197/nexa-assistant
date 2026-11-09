#include "app/ProviderGalleryDialog.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kChipColumns = 5;

// A card: the name in the text colour and bold, the line of facts under it in the muted one (the palette Nexa's theme
// sets, so both schemes and every theme pack follow). The background, hover and selection are the sheet's.
class CardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        // Not opt.text: initStyleOption turns a line break into U+2028.
        const QString text = index.data(Qt::DisplayRole).toString();
        opt.text.clear();
        const QWidget *widget = option.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const QString name = text.section(u'\n', 0, 0);
        const QString facts = text.section(u'\n', 1);
        const QRect area = opt.rect.adjusted(kPadX, kPadY, -kPadX, -kPadY);
        QFont bold = opt.font;
        bold.setBold(true);
        opt.font.setBold(false);
        painter->save();
        painter->setFont(bold);
        painter->setPen(opt.palette.color(QPalette::Text));
        painter->drawText(area, Qt::AlignLeft | Qt::AlignTop, QFontMetrics(bold).elidedText(name, Qt::ElideRight, area.width()));
        QFont regular = opt.font;
        regular.setBold(false);
        painter->setFont(regular);
        // The application's palette, not the item's: the sheet's `color` is poured into every role of the item's.
        painter->setPen(QApplication::palette().color(QPalette::PlaceholderText));
        const QFontMetrics metrics(regular);
        painter->drawText(area.adjusted(0, metrics.lineSpacing() + 2, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                          metrics.elidedText(facts, Qt::ElideRight, area.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return QSize(0, QFontMetrics(option.font).lineSpacing() * 2 + 2 + 2 * kPadY);
    }

private:
    static constexpr int kPadX = 12;
    static constexpr int kPadY = 8;
};

} // namespace

ProviderGalleryDialog::ProviderGalleryDialog(QList<ProviderPreset> presets, const QStringList &categories,
                                             QWidget *parent)
    : QDialog(parent), presets_(std::move(presets)) {
    setWindowTitle(tr("Add a provider"));
    resize(720, 600);

    for (const ProviderPreset &preset : presets_) {
        entries_.append(SearchableEntry{.id = preset.id,
                                        .name = preset.name,
                                        .category = preset.category,
                                        .host = PresetFilter::hostOf(preset.baseUrl),
                                        .freeTier = preset.freeTier,
                                        .requiresKey = preset.requiresKey});
    }

    search_ = new QLineEdit(this);
    search_->setObjectName(u"gallerySearch"_s);
    search_->setPlaceholderText(tr("Search providers — name, host, “free”, “nokey”"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    connect(search_, &QLineEdit::textChanged, this, &ProviderGalleryDialog::refill);

    // One chip per category that has a provider here, after "All"; a click picks exactly one.
    chips_ = new QButtonGroup(this);
    chips_->setExclusive(true);
    auto *chipGrid = new QGridLayout;
    chipGrid->setSpacing(6);
    QStringList shown{QString()};
    for (const QString &category : categories) {
        for (const ProviderPreset &preset : presets_) {
            if (preset.category == category) {
                shown.append(category);
                break;
            }
        }
    }
    for (int i = 0; i < shown.size(); ++i) {
        auto *chip = new QPushButton(shown.at(i).isEmpty() ? tr("All") : shown.at(i), this);
        chip->setObjectName(u"galleryChip"_s);
        chip->setCheckable(true);
        chip->setFocusPolicy(Qt::TabFocus);
        chip->setProperty("category", shown.at(i));
        chips_->addButton(chip, i);
        chipGrid->addWidget(chip, i / kChipColumns, i % kChipColumns);
        if (i == 0) chip->setChecked(true);
    }
    chipGrid->setColumnStretch(kChipColumns, 1);
    connect(chips_, &QButtonGroup::buttonClicked, this, [this](QAbstractButton *chip) {
        category_ = chip->property("category").toString();
        refill();
    });

    list_ = new QListWidget(this);
    list_->setObjectName(u"galleryList"_s);
    list_->setViewMode(QListView::ListMode);
    list_->setWordWrap(true);
    list_->setSpacing(2);
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setItemDelegate(new CardDelegate(list_));
    list_->installEventFilter(this);
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem *) { accept(); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { accept(); });
    connect(list_, &QListWidget::currentItemChanged, this, [this] {
        if (auto *add = buttons_->button(QDialogButtonBox::Ok)) add->setEnabled(list_->currentItem() != nullptr);
    });

    count_ = new QLabel(this);
    count_->setObjectName(u"galleryCount"_s);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(tr("Add"));
    connect(buttons_, &QDialogButtonBox::accepted, this, &ProviderGalleryDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &ProviderGalleryDialog::reject);

    auto *footer = new QHBoxLayout;
    footer->addWidget(count_, 1);
    footer->addWidget(buttons_);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(search_);
    layout->addLayout(chipGrid);
    layout->addWidget(list_, 1);
    layout->addLayout(footer);

    setTabOrder(search_, list_);
    search_->setFocus();
    refill();
}

QString ProviderGalleryDialog::cardText(const ProviderPreset &preset) {
    QStringList facts;
    if (ProviderRegistry::isProgramKind(preset.kind)) {
        facts.append(tr("runs on your PC"));
        facts.append(tr("sign in with your account"));
    } else {
        const QString host = PresetFilter::hostOf(preset.baseUrl);
        if (!host.isEmpty()) facts.append(host);
        facts.append(preset.requiresKey ? tr("key") : tr("no key"));
        if (preset.freeTier) facts.append(tr("free tier"));
    }
    return preset.name + u"\n"_s + facts.join(u" · "_s);
}

void ProviderGalleryDialog::refill() {
    const QList<int> shown = PresetFilter::filter(entries_, search_->text(), category_);
    list_->setUpdatesEnabled(false);
    list_->clear();
    for (const int index : shown) {
        const ProviderPreset &preset = presets_.at(index);
        auto *item = new QListWidgetItem(cardText(preset));
        item->setData(Qt::UserRole, index);
        QStringList tip;
        if (!preset.category.isEmpty()) tip.append(preset.category);
        if (!preset.hint.isEmpty()) tip.append(preset.hint);
        if (!preset.website.isEmpty()) tip.append(preset.website);
        item->setToolTip(tip.join(u"\n"_s));
        list_->addItem(item);
    }
    list_->setUpdatesEnabled(true);
    if (list_->count() > 0) list_->setCurrentRow(0);
    count_->setText(tr("%1 of %2").arg(shown.size()).arg(presets_.size()));
    if (auto *add = buttons_->button(QDialogButtonBox::Ok)) add->setEnabled(list_->count() > 0);
}

void ProviderGalleryDialog::setQuery(const QString &query) {
    search_->setText(query);
}

void ProviderGalleryDialog::setCategory(const QString &category) {
    category_ = category;
    for (QAbstractButton *chip : chips_->buttons()) {
        if (chip->property("category").toString() == category) chip->setChecked(true);
    }
    refill();
}

int ProviderGalleryDialog::shownCount() const {
    return list_->count();
}

QStringList ProviderGalleryDialog::shownIds() const {
    QStringList ids;
    for (int row = 0; row < list_->count(); ++row) {
        ids.append(presets_.at(list_->item(row)->data(Qt::UserRole).toInt()).id);
    }
    return ids;
}

void ProviderGalleryDialog::accept() {
    const QListWidgetItem *item = list_->currentItem();
    if (!item) return;
    chosen_ = presets_.at(item->data(Qt::UserRole).toInt());
    QDialog::accept();
}

bool ProviderGalleryDialog::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() != QEvent::KeyPress) return QDialog::eventFilter(watched, event);
    const int key = static_cast<QKeyEvent *>(event)->key();
    if (watched == search_) {
        // Down goes from the box into the cards; Enter takes the first, or the one the arrows reached.
        if (key == Qt::Key_Down || key == Qt::Key_PageDown) {
            if (list_->count() > 0) {
                list_->setFocus();
                if (!list_->currentItem()) list_->setCurrentRow(0);
            }
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            accept();
            return true;
        }
    } else if (watched == list_ && key == Qt::Key_Up && list_->currentRow() <= 0) {
        search_->setFocus();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace nexa
