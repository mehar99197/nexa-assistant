// app/AddTasksDialog: one task per line, the button counts them, Auto or a provider for all.

#include "app/AddTasksDialog.h"

#include "QtStringMakers.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Add tasks counts its lines and gives Auto or the provider chosen") {
    Config config;
    config.providers = {ProviderConfig{.id = u"codex"_s, .kind = u"codex"_s, .name = u"Codex"_s}};
    AddTasksDialog dialog(config);
    auto *text = dialog.findChild<QPlainTextEdit *>(u"taskLines"_s);
    auto *add = dialog.findChild<QPushButton *>(u"addTasks"_s);
    REQUIRE(text);
    REQUIRE(add);
    CHECK_FALSE(add->isEnabled());
    text->setPlainText(u"Check my inbox\n\n  Write unit tests for the auth guard  \nCompress the recordings\n"_s);
    CHECK(dialog.lines() == QStringList{u"Check my inbox"_s, u"Write unit tests for the auth guard"_s, u"Compress the recordings"_s});
    CHECK(add->text() == u"Add 3 tasks"_s);
    CHECK_FALSE(dialog.model().has_value());

    auto *provider = dialog.findChild<QComboBox *>(u"taskProvider"_s);
    auto *model = dialog.findChild<QLineEdit *>(u"taskModel"_s);
    CHECK_FALSE(model->isEnabled());
    provider->setCurrentIndex(provider->findData(u"codex"_s));
    CHECK(model->isEnabled());
    model->setText(u"gpt-6-luna"_s);
    REQUIRE(dialog.model().has_value());
    CHECK(*dialog.model() == ModelChoice{u"codex"_s, u"gpt-6-luna"_s});
}
