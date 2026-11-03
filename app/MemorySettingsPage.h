#pragma once

#include "core/Result.h"
#include "core/config/Config.h"
#include "core/memory/MemoryStore.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;

namespace nexa {

// Settings → Memory: whether Nexa remembers, whether it may save what it learns on its own, and
// every fact it holds — to read, correct, delete or add. The edits reach the memory on OK, applied
// to it as it is then (applyEdits), so what the model saved meanwhile is kept.
class MemorySettingsPage : public QWidget {
    Q_OBJECT
public:
    MemorySettingsPage(const MemoryConfig &config, MemoryStore *memory, QWidget *parent = nullptr);

    void store(MemoryConfig &config) const;
    Result<void> applyEdits() const;
    MemoryStore::Edits edits() const;

private:
    void showCount();

    MemoryStore *memory_;
    QList<MemoryEntry> shown_;          // what the list opened with
    QStringList removed_;
    QCheckBox *enabled_;
    QCheckBox *learn_;
    QListWidget *list_;
    QLabel *count_;
};

} // namespace nexa
