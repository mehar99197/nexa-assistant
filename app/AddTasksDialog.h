#pragma once

#include "core/config/Config.h"

#include <QDialog>
#include <QStringList>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace nexa {

// "Add tasks": one task per line, each run by an agent of its own, on the model each one's role is given (Auto) or on
// one chosen here for all of them.
class AddTasksDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddTasksDialog(const Config &config, QWidget *parent = nullptr);

    QStringList lines() const;                      // the lines that say something
    std::optional<ModelChoice> model() const;       // none: Auto
    void setText(const QString &text);
    // Hardcore mode's box: ticked as the window's switch is; unavailable (and why) without the root helper. `waysOut`
    // is said under it while it is ticked.
    void setHardcore(bool ticked, bool available, const QString &waysOut, const QString &unavailableWhy = {});
    bool hardcore() const;                          // lock the keyboard and mouse while these run

private:
    void refresh();

    QPlainTextEdit *text_;
    QComboBox *provider_;
    QLineEdit *model_;
    QCheckBox *hardcore_;
    QLabel *hardcoreNote_;
    QPushButton *add_;
};

} // namespace nexa
