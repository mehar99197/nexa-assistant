#pragma once

#include "core/ai/PresetFilter.h"
#include "core/ai/ProviderRegistry.h"

#include <QDialog>
#include <QList>
#include <QStringList>

#include <optional>

class QButtonGroup;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace nexa {

// Mediator between PresetFilter and a list widget: every provider Nexa can add, as a searchable list: a search box (name, host, "free", "nokey"), category chips
// and one card per preset — name, then "host · key | no key · free tier"; a program says it runs on this PC. What
// to show and in which order is PresetFilter's (pure, tested); this only draws it. Choosing a card — Enter,
// double-click or Add — accepts, and selectedPreset() is the one chosen. Colours and shapes come from theme.qss.
class ProviderGalleryDialog : public QDialog {
    Q_OBJECT
public:
    // `presets` as ProviderRegistry::availablePresets() gives them, `categories` in display order (the chips).
    ProviderGalleryDialog(QList<ProviderPreset> presets, const QStringList &categories, QWidget *parent = nullptr);

    // The card chosen when the dialog was accepted; none if it was closed.
    std::optional<ProviderPreset> selectedPreset() const { return chosen_; }

    // The tests and the pictures drive it through these.
    void setQuery(const QString &query);
    void setCategory(const QString &category);       // empty: All
    int shownCount() const;
    QStringList shownIds() const;

    void accept() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refill();
    static QString cardText(const ProviderPreset &preset);

    QList<ProviderPreset> presets_;
    QList<SearchableEntry> entries_;
    QString category_;
    QLineEdit *search_{nullptr};
    QListWidget *list_{nullptr};
    QLabel *count_{nullptr};
    QButtonGroup *chips_{nullptr};
    QDialogButtonBox *buttons_{nullptr};
    std::optional<ProviderPreset> chosen_;
};

} // namespace nexa
