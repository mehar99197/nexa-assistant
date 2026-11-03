#include "core/policy/PolicyEngine.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

const ToolSpec kRunCommand{u"run_command"_s, {}, {}, true, false};
const ToolSpec kRootCommand{u"run_root_command"_s, {}, {}, true, true};
const ToolSpec kReadFile{u"read_file"_s, {}, {}, false, false};

QJsonObject command(const QString &line) {
    return QJsonObject{{u"command"_s, line}};
}

} // namespace

TEST_CASE("PolicyEngine flags destructive commands") {
    for (const QString &line : {u"rm -rf /tmp/build"_s, u"rm -r old"_s, u"sudo rm -f notes.txt"_s, u"rm -v -R dir"_s,
                                u"mkfs.ext4 /dev/sdb1"_s, u"dd if=/dev/zero of=/dev/sda bs=1M"_s, u"shutdown now"_s,
                                u"systemctl reboot"_s, u"git reset --hard HEAD~1"_s, u"git clean -fdx"_s,
                                u"find . -name '*.o' -delete"_s, u"apt-get purge firefox-esr"_s, u"kill -9 -1"_s,
                                u":(){ :|:& };:"_s, u"echo x > /dev/sda"_s, u"chmod -R 777 /"_s}) {
        INFO(line.toStdString());
        CHECK_FALSE(PolicyEngine::destructiveReason(line).isEmpty());
    }
}

TEST_CASE("PolicyEngine judges typed text like the command it becomes") {
    ToolSpec typing{u"type_text"_s, u"Type"_s, {}};
    typing.privileged = true;
    typing.risk = u"types on your keyboard"_s;
    const PolicyEngine autonomous(PolicyMode::Autonomous);
    const PolicyDecision wipe = autonomous.evaluate(typing, {{u"text"_s, u"rm -rf ~/work\n"_s}});
    CHECK(wipe.destructive);
    CHECK_FALSE(wipe.reason.isEmpty());
    const PolicyDecision list = autonomous.evaluate(typing, {{u"text"_s, u"ls -la\n"_s}});
    CHECK_FALSE(list.destructive);
    CHECK(list.reason == u"types on your keyboard"_s);
}

TEST_CASE("PolicyEngine leaves ordinary commands alone") {
    for (const QString &line : {u"rm notes.txt"_s, u"ls -la"_s, u"rmdir empty"_s, u"grep -r TODO ."_s,
                                u"git status"_s, u"apt install vim"_s, u"kill 1234"_s, u"df -h"_s,
                                u"chmod 644 file"_s, u"find . -name '*.cc'"_s}) {
        INFO(line.toStdString());
        CHECK(PolicyEngine::destructiveReason(line).isEmpty());
    }
}

TEST_CASE("PolicyEngine: autonomous mode never asks but still flags") {
    const PolicyEngine policy(PolicyMode::Autonomous);
    const PolicyDecision decision = policy.evaluate(kRunCommand, command(u"rm -rf ~/old"_s));
    CHECK_FALSE(decision.needsConfirmation);
    CHECK(decision.destructive);
    CHECK_FALSE(policy.evaluate(kRootCommand, command(u"apt update"_s)).needsConfirmation);
}

TEST_CASE("PolicyEngine: confirm-risky asks for root and destructive actions only") {
    const PolicyEngine policy(PolicyMode::ConfirmRisky);
    CHECK(policy.evaluate(kRunCommand, command(u"rm -rf ~/old"_s)).needsConfirmation);
    CHECK(policy.evaluate(kRootCommand, command(u"apt update"_s)).needsConfirmation);
    CHECK(policy.evaluate(kRootCommand, command(u"apt update"_s)).reason == u"runs as root"_s);
    CHECK_FALSE(policy.evaluate(kRunCommand, command(u"ls"_s)).needsConfirmation);
}

TEST_CASE("PolicyEngine: confirm-all asks for every mutating action") {
    const PolicyEngine policy(PolicyMode::ConfirmAll);
    CHECK(policy.evaluate(kRunCommand, command(u"ls"_s)).needsConfirmation);
    CHECK_FALSE(policy.evaluate(kReadFile, QJsonObject{{u"path"_s, u"~/a.txt"_s}}).needsConfirmation);
}

TEST_CASE("PolicyEngine mode names round-trip") {
    for (const PolicyMode mode : {PolicyMode::Autonomous, PolicyMode::ConfirmRisky, PolicyMode::ConfirmAll}) {
        CHECK(PolicyEngine::modeFromString(PolicyEngine::modeToString(mode)) == mode);
    }
    CHECK(PolicyEngine::modeFromString(u"nonsense"_s) == PolicyMode::Autonomous);
}

TEST_CASE("PolicyEngine: a browser click asks in confirm-risky mode with its own risk; typed page text is not a command") {
    const ToolSpec click{u"browser_click"_s, u"Click"_s, {}, true, true, u"acts on web pages in your browser, signed in as you"_s};
    const ToolSpec type{u"browser_type"_s, u"Type"_s, {}, true, true, u"acts on web pages in your browser, signed in as you"_s};
    const ToolSpec navigate{u"browser_navigate"_s, u"Go"_s, {}, true, false, {}};
    const PolicyEngine risky(PolicyMode::ConfirmRisky);
    const PolicyDecision clicked = risky.evaluate(click, QJsonObject{{u"target"_s, u"e42"_s}});
    CHECK(clicked.needsConfirmation);
    CHECK(clicked.reason == click.risk);
    CHECK_FALSE(risky.evaluate(navigate, QJsonObject{{u"url"_s, u"https://example.com"_s}}).needsConfirmation);
    // Text typed into a page is not judged as a shell command (only type_text's is).
    CHECK_FALSE(risky.evaluate(type, QJsonObject{{u"text"_s, u"rm -rf /"_s}}).destructive);
    CHECK_FALSE(PolicyEngine(PolicyMode::Autonomous).evaluate(click, {}).needsConfirmation);
}
