#pragma once

#include "core/config/Config.h"

#include <QDialog>
#include <QStringList>

#include <optional>

class QComboBox;
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

private:
    void refresh();

    QPlainTextEdit *text_;
    QComboBox *provider_;
    QLineEdit *model_;
    QPushButton *add_;
};

} // namespace nexa
