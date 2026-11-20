// core/ai/OpenCodeSignIn — opencode's own browser sign-ins from inside Nexa — and the pure sign-in pieces of
// OpenCodeWire, against tests/fake-opencode/opencode (a stand-in for `opencode serve`, 1.19.3's API). No test
// here starts the real opencode, signs in to anything or touches ~/.local/share/opencode.

#include "core/ai/OpenCodeServer.h"
#include "core/ai/OpenCodeSignIn.h"
#include "core/ai/OpenCodeWire.h"
#include "core/net/HttpClient.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>
#include <vector>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 20'000;
#ifdef Q_OS_WIN
const QString kFake = QStringLiteral(NEXA_TESTS_DIR "/fake-opencode/opencode.cmd");
const QString kSilent = QStringLiteral(NEXA_TESTS_DIR "/fake-opencode/silent.cmd");
#else
const QString kFake = QStringLiteral(NEXA_TESTS_DIR "/fake-opencode/opencode");
const QString kSilent = QStringLiteral(NEXA_TESTS_DIR "/fake-opencode/silent");
#endif

// GET /provider/auth of a clean opencode 1.19.3 (its own sandboxed server, 2026-10-31), plus "google" as the
// owner's opencode-antigravity-auth plugin adds it.
const QByteArray kCleanAuth =
    R"json({"openai":[{"type":"oauth","label":"ChatGPT Pro/Plus (browser)"},{"type":"oauth","label":"ChatGPT Pro/Plus (headless)"},{"type":"api","label":"Manually enter API Key"}],)json"
    R"json("github-copilot":[{"type":"oauth","label":"Login with GitHub Copilot","prompts":[{"type":"select","key":"deploymentType","message":"Select GitHub deployment type","options":[{"label":"GitHub.com","value":"github.com","hint":"Public"},{"label":"GitHub Enterprise","value":"enterprise","hint":"Data residency or self-hosted"}]},{"type":"text","key":"enterpriseUrl","message":"Enter your GitHub Enterprise URL or domain","placeholder":"company.ghe.com or https://company.ghe.com","when":{"key":"deploymentType","op":"eq","value":"enterprise"}}]}],)json"
    R"json("gitlab":[{"type":"oauth","label":"GitLab OAuth","prompts":[{"type":"text","key":"instanceUrl","message":"GitLab instance URL","placeholder":"https://gitlab.com"}]},{"type":"api","label":"GitLab Personal Access Token","prompts":[{"type":"text","key":"instanceUrl","message":"GitLab instance URL","placeholder":"https://gitlab.com"}]}],)json"
    R"json("poe":[{"type":"oauth","label":"Login with Poe (browser)"},{"type":"api","label":"Manually enter API Key"}],)json"
    R"json("cloudflare-workers-ai":[{"type":"api","label":"API key","prompts":[{"type":"text","key":"accountId","message":"Enter your Cloudflare Account ID","placeholder":"e.g. 1234567890abcdef1234567890abcdef"}]}],)json"
    R"json("cloudflare-ai-gateway":[{"type":"api","label":"Gateway API token","prompts":[{"type":"text","key":"accountId","message":"Enter your Cloudflare Account ID","placeholder":"e.g. 1234567890abcdef1234567890abcdef"},{"type":"text","key":"gatewayId","message":"Enter your Cloudflare AI Gateway ID","placeholder":"e.g. my-gateway"}]}],)json"
    R"json("azure":[{"type":"api","label":"API key","prompts":[{"type":"text","key":"resourceName","message":"Enter Azure Resource Name","placeholder":"e.g. my-models"}]},{"type":"oauth","label":"Microsoft Entra ID (Azure CLI)","prompts":[{"type":"text","key":"resourceName","message":"Enter Azure Resource Name","placeholder":"e.g. my-models"}]}],)json"
    R"json("digitalocean":[{"type":"oauth","label":"Login with DigitalOcean"},{"type":"api","label":"Paste Model Access Key"}],)json"
    R"json("snowflake-cortex":[{"type":"oauth","label":"Login with Snowflake (External Browser)","prompts":[{"type":"text","key":"account","message":"Snowflake Account Identifier","placeholder":"myorg-myaccount"},{"type":"text","key":"role","message":"Snowflake Role (optional)","placeholder":"PUBLIC"}]},{"type":"api","label":"Paste PAT or bearer token manually","prompts":[{"type":"text","key":"account","message":"Snowflake Account Identifier","placeholder":"myorg-myaccount"}]}],)json"
    R"json("xai":[{"type":"oauth","label":"SuperGrok Subscription"},{"type":"api","label":"Manually enter API Key"}],)json"
    R"json("google":[{"type":"oauth","label":"OAuth with Google (Antigravity)"},{"type":"api","label":"Manually enter API Key"}]})json";

QJsonObject object(const QByteArray &json) { return QJsonDocument::fromJson(json).object(); }

QStringList ids(const QList<SignInMethod> &methods) {
    QStringList found;
    for (const SignInMethod &method : methods) found.append(method.id);
    return found;
}

// ---- the rig: a fake opencode scripted by a JSON file, and what an ISignIn said ----

struct SignInRig {
    explicit SignInRig(const QJsonObject &script = {}) {
        QFile file(dir.filePath(u"auth.json"_s));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(script).toJson());
        qputenv("FAKE_OPENCODE_AUTH", file.fileName().toUtf8());
        qputenv("FAKE_OPENCODE_LOG", dir.filePath(u"log.jsonl"_s).toUtf8());
    }
    ~SignInRig() {
        qunsetenv("FAKE_OPENCODE_AUTH");
        qunsetenv("FAKE_OPENCODE_LOG");
    }

    QList<QJsonObject> log() const {
        QList<QJsonObject> entries;
        QFile file(dir.filePath(u"log.jsonl"_s));
        if (!file.open(QIODevice::ReadOnly)) return entries;
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (!line.isEmpty()) entries.append(QJsonDocument::fromJson(line).object());
        }
        return entries;
    }
    // "METHOD /path" of every request the server logged after its start line, in order.
    QStringList requests() const {
        QStringList seen;
        for (const QJsonObject &entry : log()) {
            if (entry.contains(u"path"_s)) {
                seen.append(entry.value(u"method"_s).toString() + u' ' + entry.value(u"path"_s).toString());
            }
        }
        return seen;
    }
    QList<QJsonObject> requests(const QString &method, const QString &path) const {
        QList<QJsonObject> found;
        for (const QJsonObject &entry : log()) {
            if (entry.value(u"method"_s).toString() == method && entry.value(u"path"_s).toString() == path) {
                found.append(entry);
            }
        }
        return found;
    }
    std::shared_ptr<OpenCodeServer> server(const QString &program = kFake) {
        return std::make_shared<OpenCodeServer>(&http, program, QUrl(), QString(), dir.filePath(u"work"_s));
    }

    QTemporaryDir dir;
    HttpClient http;
};

struct Seen {
    explicit Seen(OpenCodeSignIn &signIn) {
        QObject::connect(&signIn, &ISignIn::statusChecked, [this](const SignInStatus &value) { status = value; });
        QObject::connect(&signIn, &ISignIn::methodsListed,
                         [this](const QList<SignInMethod> &value) { methods = value; });
        QObject::connect(&signIn, &ISignIn::linkReady, [this, &signIn](const QUrl &url, const QString &words, bool opened) {
            link = Link{url, words, opened};
            needsCodeAtLink = signIn.pendingNeedsCode();
        });
        QObject::connect(&signIn, &ISignIn::signedIn, [this](const SignInStatus &value) { signedIn = value; });
        QObject::connect(&signIn, &ISignIn::signedOut, [this] { ++signedOut; });
        QObject::connect(&signIn, &ISignIn::failed, [this](const Error &error) { failures.append(error); });
    }

    struct Link {
        QUrl url;
        QString instructions;
        bool browserOpened{true};
    };
    std::optional<SignInStatus> status;
    std::optional<QList<SignInMethod>> methods;
    std::optional<Link> link;
    bool needsCodeAtLink{false};
    std::optional<SignInStatus> signedIn;
    int signedOut{0};
    QList<Error> failures;

    bool anyEnd() const { return status || methods || signedIn || signedOut > 0 || !failures.isEmpty(); }
    void clear() {
        status.reset();
        methods.reset();
        link.reset();
        needsCodeAtLink = false;
        signedIn.reset();
        signedOut = 0;
        failures.clear();
    }
};

// Everything Nexa logs while it lives, with the debug categories on.
struct LogCapture {
    LogCapture() {
        lines().clear();
        previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            lines().append(message);
        });
        QLoggingCategory::setFilterRules(u"nexa.*.debug=true"_s);
    }
    ~LogCapture() {
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(previous);
    }
    static QStringList &lines() {
        static QStringList captured;
        return captured;
    }
    QtMessageHandler previous{nullptr};
};

QJsonObject autoReply(const QString &url = u"https://auth.example.test/authorize?state=SECRETSTATE"_s) {
    return {{u"url"_s, url}, {u"method"_s, u"auto"_s}, {u"instructions"_s, u"Complete authorization in your browser."_s}};
}

} // namespace

// ============================== OpenCodeWire: the sign-in pieces (pure) ==============================

TEST_CASE("GET /provider/auth lists every way to connect, by provider and index", "[opencode-signin][pure]") {
    const Result<QList<OpenCodeAuthMethod>> parsed = OpenCodeWire::parseAuthMethods(kCleanAuth);
    REQUIRE(parsed.isOk());
    QHash<QString, QList<OpenCodeAuthMethod>> byProvider;
    for (const OpenCodeAuthMethod &method : parsed.value()) byProvider[method.providerId].append(method);
    CHECK(byProvider.size() == 11);

    const QList<OpenCodeAuthMethod> openai = byProvider.value(u"openai"_s);
    REQUIRE(openai.size() == 3);
    CHECK(openai[0].index == 0);
    CHECK(openai[0].oauth);
    CHECK(openai[0].label == u"ChatGPT Pro/Plus (browser)"_s);
    CHECK_FALSE(openai[0].hasPrompts);
    CHECK(openai[1].oauth);
    CHECK(openai[1].label == u"ChatGPT Pro/Plus (headless)"_s);
    CHECK(openai[2].index == 2);
    CHECK_FALSE(openai[2].oauth);               // an API key: not a browser sign-in
    // Methods that ask questions first say so.
    CHECK(byProvider.value(u"github-copilot"_s).first().hasPrompts);
    CHECK(byProvider.value(u"azure"_s).at(1).oauth);
    CHECK(byProvider.value(u"azure"_s).at(1).hasPrompts);
    CHECK_FALSE(byProvider.value(u"azure"_s).at(0).oauth);
}

TEST_CASE("The sign-in list is read tolerantly: unknown fields, an empty prompts list, entries of an odd shape", "[opencode-signin][pure]") {
    const QByteArray body = R"({
        "openai": [{"type": "oauth", "label": "A", "prompts": [], "future": {"x": 1}}, 7, "text",
                   {"label": "no type"}, {"type": "oauth"}],
        "poe": "not a list", "xai": null})";
    const Result<QList<OpenCodeAuthMethod>> parsed = OpenCodeWire::parseAuthMethods(body);
    REQUIRE(parsed.isOk());
    REQUIRE(parsed.value().size() == 2);        // the first and the last: type "oauth" with or without a label
    CHECK(parsed.value()[0].providerId == u"openai"_s);
    CHECK(parsed.value()[0].index == 0);
    CHECK_FALSE(parsed.value()[0].hasPrompts);  // an empty list asks nothing
    CHECK(parsed.value()[1].index == 4);        // the index counts every entry, odd ones included

    CHECK(OpenCodeWire::parseAuthMethods("{}").isOk());
    CHECK(OpenCodeWire::parseAuthMethods("{}").value().isEmpty());
    for (const QByteArray &bad : {QByteArray("not json"), QByteArray("[]"), QByteArray("null"), QByteArray()}) {
        const Result<QList<OpenCodeAuthMethod>> failed = OpenCodeWire::parseAuthMethods(bad);
        REQUIRE_FALSE(failed.isOk());
        CHECK(failed.error().code == ErrorCode::Parse);
    }
}

TEST_CASE("authorize and callback bodies carry the method index, and a code only when there is one", "[opencode-signin][pure]") {
    CHECK(object(OpenCodeWire::authorizeBody(1)) == QJsonObject{{u"method"_s, 1}});
    CHECK(object(OpenCodeWire::callbackBody(0)) == QJsonObject{{u"method"_s, 0}});
    CHECK(object(OpenCodeWire::callbackBody(2, u"AB-12"_s)) == QJsonObject{{u"method"_s, 2}, {u"code"_s, u"AB-12"_s}});
}

TEST_CASE("The authorize reply: an auto or a code flow, or why it is none", "[opencode-signin][pure]") {
    const Result<OpenCodeAuthorization> autoFlow = OpenCodeWire::parseAuthorization(
        R"({"url":"https://auth.openai.com/oauth/authorize?state=x","method":"auto","instructions":"Complete it."})");
    REQUIRE(autoFlow.isOk());
    CHECK(autoFlow.value().url == QUrl(u"https://auth.openai.com/oauth/authorize?state=x"_s));
    CHECK_FALSE(autoFlow.value().needsCode);
    CHECK(autoFlow.value().instructions == u"Complete it."_s);

    const Result<OpenCodeAuthorization> codeFlow = OpenCodeWire::parseAuthorization(
        R"({"url":"http://127.0.0.1:9/cb","method":"code","instructions":"Enter the code"})");
    REQUIRE(codeFlow.isOk());
    CHECK(codeFlow.value().needsCode);

    // An empty instructions text is fine; the null body of a method that is no oauth one is NotSupported.
    CHECK(OpenCodeWire::parseAuthorization(R"({"url":"https://a.test/x","method":"auto"})").isOk());
    const Result<OpenCodeAuthorization> none = OpenCodeWire::parseAuthorization("null");
    REQUIRE_FALSE(none.isOk());
    CHECK(none.error().code == ErrorCode::NotSupported);
    CHECK(OpenCodeWire::parseAuthorization(" null\n").error().code == ErrorCode::NotSupported);

    // Everything else is Parse — and a url that is not a web page is never handed on to be opened.
    for (const char *bad : {"", "text", "[]", "{}", R"({"url":"https://a.test","method":"device"})",
                            R"({"url":"https://a.test"})", R"({"method":"auto","url":""})",
                            R"j({"url":"javascript:alert(1)","method":"auto"})j", R"({"url":"file:///etc/passwd","method":"auto"})",
                            R"({"url":"https://","method":"auto"})", R"({"url":7,"method":"auto"})"}) {
        const Result<OpenCodeAuthorization> failed = OpenCodeWire::parseAuthorization(bad);
        INFO(bad);
        REQUIRE_FALSE(failed.isOk());
        CHECK(failed.error().code == ErrorCode::Parse);
    }
}

TEST_CASE("The callback's answer is true only for the body true", "[opencode-signin][pure]") {
    CHECK(OpenCodeWire::isTrue("true"));
    CHECK(OpenCodeWire::isTrue(" true\n"));
    CHECK_FALSE(OpenCodeWire::isTrue("false"));
    CHECK_FALSE(OpenCodeWire::isTrue("null"));
    CHECK_FALSE(OpenCodeWire::isTrue(""));
    CHECK_FALSE(OpenCodeWire::isTrue(R"({"name":"x"})"));
}

TEST_CASE("The connected providers come from GET /provider or GET /config/providers, and nothing else is 'none'", "[opencode-signin][pure]") {
    const Result<QStringList> fromProvider =
        OpenCodeWire::parseConnected(R"({"all":[{"id":"x"}],"default":{},"connected":["opencode","openai"]})");
    REQUIRE(fromProvider.isOk());
    CHECK(fromProvider.value() == QStringList{u"opencode"_s, u"openai"_s});

    const Result<QStringList> fromConfig = OpenCodeWire::parseConnected(
        R"({"providers":[{"id":"github-copilot","models":{}},{"id":"opencode"},{"nope":1}],"default":{}})");
    REQUIRE(fromConfig.isOk());
    CHECK(fromConfig.value() == QStringList{u"github-copilot"_s, u"opencode"_s});

    CHECK(OpenCodeWire::parseConnected(R"({"providers":[],"default":{}})").value().isEmpty());
    CHECK(OpenCodeWire::parseConnected(R"({"connected":[]})").value().isEmpty());
    for (const char *bad : {"", "[]", "null", "{}", R"({"providers":"x"})", "<html>"}) {
        const Result<QStringList> failed = OpenCodeWire::parseConnected(bad);
        INFO(bad);
        REQUIRE_FALSE(failed.isOk());
        CHECK(failed.error().code == ErrorCode::Parse);
    }
}

TEST_CASE("opencode's sign-in errors become sentences, without urls, codes or long server text", "[opencode-signin][pure]") {
    const auto failure = [](const QByteArray &body, OpenCodeAuthStep step, int status = 400) {
        return OpenCodeWire::authFailure(status, body, step);
    };
    const Error missing = failure(R"({"name":"ProviderAuthOauthMissing","data":{"providerID":"openai"}})",
                                  OpenCodeAuthStep::WaitForBrowser);
    CHECK(missing.code == ErrorCode::NotFound);
    CHECK(missing.message.contains(u"Start it again"_s));

    CHECK(failure(R"({"name":"ProviderAuthOauthCodeMissing","data":{}})", OpenCodeAuthStep::SubmitCode).code
          == ErrorCode::InvalidArgument);
    const Error code = failure(R"({"name":"ProviderAuthOauthCallbackFailed","data":{}})", OpenCodeAuthStep::SubmitCode);
    CHECK(code.code == ErrorCode::Unauthorized);
    CHECK(code.message.contains(u"code"_s));
    const Error browser = failure(R"({"name":"ProviderAuthOauthCallbackFailed","data":{}})", OpenCodeAuthStep::WaitForBrowser);
    CHECK(browser.code == ErrorCode::Unauthorized);
    CHECK(browser.message.contains(u"timed out"_s));
    CHECK_FALSE(browser.message == code.message);

    const Error validation = failure(
        R"({"name":"ProviderAuthValidationFailed","data":{"field":"instanceUrl","message":"bad https://x.test/?code=SECRET"}})",
        OpenCodeAuthStep::Authorize);
    CHECK(validation.code == ErrorCode::InvalidArgument);
    CHECK(validation.message.contains(u"instanceUrl"_s));
    CHECK_FALSE(validation.message.contains(u"SECRET"_s));

    // opencode's own words are kept short and never carry a link (the url's query holds state and codes).
    const Error provider = failure(
        R"({"name":"ProviderAuthError","data":{"providerID":"openai","message":"Failed at https://auth.openai.com/x?code=SECRETCODE&state=s now"}})",
        OpenCodeAuthStep::Authorize);
    CHECK(provider.code == ErrorCode::Unauthorized);
    CHECK(provider.message.contains(u"Failed at"_s));
    CHECK_FALSE(provider.message.contains(u"SECRETCODE"_s));
    CHECK_FALSE(provider.message.contains(u"https"_s));
    const QString longText = QString(5000, u'x');
    CHECK(failure(QJsonDocument(QJsonObject{{u"name"_s, u"ProviderAuthError"_s},
                                            {u"data"_s, QJsonObject{{u"message"_s, longText}}}}).toJson(),
                  OpenCodeAuthStep::Authorize).message.size() < 400);

    // Effect's own errors, an error nobody knows, an answer that is no JSON.
    CHECK(failure(R"({"_tag":"BadRequest"})", OpenCodeAuthStep::Authorize).code == ErrorCode::InvalidArgument);
    CHECK(failure(R"({"_tag":"InvalidRequestError","message":"method must be a number"})", OpenCodeAuthStep::Authorize)
              .message.contains(u"method must be a number"_s));
    const Error odd = failure(R"({"name":"SomethingNew","data":{}})", OpenCodeAuthStep::WaitForBrowser);
    CHECK(odd.code == ErrorCode::Http);
    CHECK(odd.message.contains(u"SomethingNew"_s));
    CHECK(odd.message.contains(u"400"_s));
    const Error crashed = failure("boom <script>", OpenCodeAuthStep::Authorize, 500);
    CHECK(crashed.code == ErrorCode::Http);
    CHECK(crashed.message.contains(u"500"_s));
    CHECK(crashed.message.contains(u"another program"_s));       // the likely reason: port 1455 taken
    CHECK_FALSE(crashed.message.contains(u"script"_s));
    CHECK(failure("", OpenCodeAuthStep::SubmitCode, 401).code == ErrorCode::Unauthorized);
}

// ============================== OpenCodeSignIn: what Nexa offers (pure) ==============================

TEST_CASE("The allow-list is data: lower-case unique provider ids, each with a name", "[opencode-signin][pure]") {
    const QList<OpenCodeSignIn::Provider> &providers = OpenCodeSignIn::providers();
    CHECK(providers.size() == 8);
    QSet<QString> seen;
    for (const OpenCodeSignIn::Provider &provider : providers) {
        CHECK_FALSE(provider.name.isEmpty());
        CHECK(provider.id == provider.id.toLower());
        CHECK_FALSE(provider.id.contains(u'#'));                // '#' separates the index in a method id
        CHECK_FALSE(seen.contains(provider.id));
        seen.insert(provider.id);
    }
    CHECK(seen.contains(u"openai"_s));
    CHECK_FALSE(seen.contains(u"google"_s));                    // a plugin's, not opencode's own
}

TEST_CASE("Nexa offers opencode's own browser sign-ins that ask no questions, and ignores a plugin's", "[opencode-signin][pure]") {
    const Result<QList<OpenCodeAuthMethod>> parsed = OpenCodeWire::parseAuthMethods(kCleanAuth);
    REQUIRE(parsed.isOk());
    const QList<SignInMethod> offered = OpenCodeSignIn::offeredMethods(parsed.value());
    // In the allow-list's order; GitHub Copilot, GitLab, Snowflake and Azure's Entra ID each ask first (a later
    // step); no API key entry; "google" is a plugin's.
    CHECK(ids(offered) == QStringList{u"openai#0"_s, u"openai#1"_s, u"poe#0"_s, u"digitalocean#0"_s, u"xai#0"_s});
    REQUIRE(offered.size() == 5);
    CHECK(offered[0].label == u"OpenAI · ChatGPT Pro/Plus (browser)"_s);
    CHECK(offered[1].label == u"OpenAI · ChatGPT Pro/Plus (headless)"_s);
    CHECK(offered[2].label == u"Login with Poe (browser)"_s);   // already names the provider
    CHECK(offered[3].label == u"Login with DigitalOcean"_s);
    CHECK(offered[4].label == u"xAI · SuperGrok Subscription"_s);
    for (const SignInMethod &method : offered) CHECK_FALSE(method.needsCode);  // learnt after authorize
}

TEST_CASE("A method id is <provider>#<index>", "[opencode-signin][pure]") {
    QString provider;
    int index = -1;
    REQUIRE(OpenCodeSignIn::parseMethodId(u"openai#1"_s, &provider, &index));
    CHECK(provider == u"openai"_s);
    CHECK(index == 1);
    REQUIRE(OpenCodeSignIn::parseMethodId(u"github-copilot#0"_s, &provider, &index));
    CHECK(provider == u"github-copilot"_s);
    for (const QString &bad : {QString(), u"openai"_s, u"#0"_s, u"openai#"_s, u"openai#x"_s, u"openai#-1"_s,
                               u"openai#1#2"_s, u"openai #1"_s, u"openai#1 "_s}) {
        INFO(bad.toStdString());
        CHECK_FALSE(OpenCodeSignIn::parseMethodId(bad, &provider, &index));
    }
}

TEST_CASE("The status says which of the offered providers are connected, by name", "[opencode-signin][pure]") {
    const SignInStatus none = OpenCodeSignIn::statusFor({u"opencode"_s, u"deepseek"_s});
    CHECK(none.state == SignInStatus::State::SignedOut);
    CHECK(none.detail.isEmpty());
    const SignInStatus some = OpenCodeSignIn::statusFor({u"xai"_s, u"google"_s, u"github-copilot"_s, u"opencode"_s});
    CHECK(some.state == SignInStatus::State::SignedIn);
    CHECK(some.detail == u"GitHub Copilot, xAI"_s);             // the allow-list's order; no plugin's provider
    CHECK(OpenCodeSignIn::statusFor({}).state == SignInStatus::State::SignedOut);
}

// ============================== OpenCodeSignIn against the fake opencode ==============================

TEST_CASE("The methods are listed from the server's own list; nothing is authorized to list them", "[opencode-signin][facade]") {
    SignInRig rig;
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    CHECK_FALSE(signIn.busy());
    signIn.listMethods();
    CHECK(signIn.busy());
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    REQUIRE(seen.methods);
    CHECK(seen.failures.isEmpty());
    CHECK(ids(*seen.methods) == QStringList{u"openai#0"_s, u"openai#1"_s, u"poe#0"_s, u"digitalocean#0"_s, u"xai#0"_s});
    CHECK_FALSE(signIn.busy());
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/authorize"_s).isEmpty());
    CHECK(rig.requests().contains(u"GET /provider/auth"_s));
}

TEST_CASE("A provider a plugin added is not offered, whatever the server lists", "[opencode-signin][facade]") {
    const QJsonObject providers{
        {u"openai"_s, QJsonArray{QJsonObject{{u"type"_s, u"oauth"_s}, {u"label"_s, u"ChatGPT Pro/Plus (browser)"_s}}}},
        {u"my-plugin"_s, QJsonArray{QJsonObject{{u"type"_s, u"oauth"_s}, {u"label"_s, u"Login with a plugin"_s}}}},
        {u"google"_s, QJsonArray{QJsonObject{{u"type"_s, u"oauth"_s}, {u"label"_s, u"OAuth with Google (Antigravity)"_s}}}}};
    SignInRig rig({{u"providers"_s, providers}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.listMethods();
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    REQUIRE(seen.methods);
    CHECK(ids(*seen.methods) == QStringList{u"openai#0"_s});

    // Nor can one be signed in to by naming it.
    signIn.signIn(u"my-plugin#0"_s);
    REQUIRE(QTest::qWaitFor([&] { return !seen.failures.isEmpty(); }, kWaitMs));
    CHECK(seen.failures.first().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(seen.link);
    CHECK(rig.requests(u"POST"_s, u"/provider/my-plugin/oauth/authorize"_s).isEmpty());
}

TEST_CASE("A server that does not start fails every question and claims nothing", "[opencode-signin][facade]") {
    SignInRig rig;
    std::shared_ptr<OpenCodeServer> server = rig.server(kSilent);
    server->setStartTimeoutMs(500);
    OpenCodeSignIn signIn(server);
    Seen seen(signIn);

    signIn.checkStatus();
    REQUIRE(QTest::qWaitFor([&] { return !seen.failures.isEmpty(); }, kWaitMs));
    REQUIRE(seen.status);                                       // Unknown, then the failure — never "signed out"
    CHECK(seen.status->state == SignInStatus::State::Unknown);
    CHECK(seen.failures.first().code == ErrorCode::Timeout);
    CHECK(seen.failures.size() == 1);
    CHECK_FALSE(signIn.busy());

    // Asked again once the killed child is gone (OpenCodeServer starts a new one over a still-running one
    // otherwise: not this class's to settle).
    REQUIRE(QTest::qWaitFor([&] { return server->processId() == 0; }, kWaitMs));
    seen.clear();
    signIn.listMethods();
    REQUIRE(QTest::qWaitFor([&] { return !seen.failures.isEmpty(); }, kWaitMs));
    CHECK_FALSE(seen.methods);
    CHECK(seen.failures.first().code == ErrorCode::Timeout);
}

TEST_CASE("A missing opencode is NotFound, not 'signed out'", "[opencode-signin][facade]") {
    SignInRig rig;
    OpenCodeSignIn signIn(rig.server(QString()));
    Seen seen(signIn);
    signIn.checkStatus();
    REQUIRE(QTest::qWaitFor([&] { return !seen.failures.isEmpty(); }, kWaitMs));
    CHECK(seen.failures.first().code == ErrorCode::NotFound);
    REQUIRE(seen.status);
    CHECK(seen.status->state == SignInStatus::State::Unknown);
}

TEST_CASE("Status: signed out with only the free provider connected, signed in by name otherwise", "[opencode-signin][facade]") {
    SECTION("signed out") {
        SignInRig rig({{u"connected"_s, QJsonArray{u"opencode"_s, u"deepseek"_s}}});
        std::shared_ptr<OpenCodeServer> server = rig.server();
        OpenCodeSignIn signIn(server);
        bool readyWhenSaid = false;
        QObject::connect(&signIn, &ISignIn::statusChecked, [&] { readyWhenSaid = server->isReady(); });
        Seen seen(signIn);
        signIn.checkStatus();
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.status);
        CHECK(seen.status->state == SignInStatus::State::SignedOut);
        CHECK(readyWhenSaid);                                   // nothing is claimed before the server is up
        CHECK(seen.failures.isEmpty());
    }
    SECTION("signed in") {
        SignInRig rig({{u"connected"_s, QJsonArray{u"github-copilot"_s, u"google"_s, u"opencode"_s}}});
        OpenCodeSignIn signIn(rig.server());
        Seen seen(signIn);
        signIn.checkStatus();
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.status);
        CHECK(seen.status->state == SignInStatus::State::SignedIn);
        CHECK(seen.status->detail == u"GitHub Copilot"_s);      // google is a plugin's: not Nexa's to report
        // The cheap list, not /provider's 7 MB.
        CHECK(rig.requests().contains(u"GET /config/providers"_s));
        CHECK_FALSE(rig.requests().contains(u"GET /provider"_s));
    }
}

TEST_CASE("An 'auto' sign-in shows the link at once, waits for the browser and reports the plan it reached", "[opencode-signin][facade]") {
    SignInRig rig({{u"authorize"_s, QJsonObject{{u"openai/0"_s, autoReply()}}},
                   {u"callback"_s, QJsonObject{{u"delay"_s, 0.4}}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signIn(u"openai#0"_s);
    CHECK(signIn.busy());

    REQUIRE(QTest::qWaitFor([&] { return seen.link.has_value(); }, kWaitMs));
    CHECK(seen.link->url == QUrl(u"https://auth.example.test/authorize?state=SECRETSTATE"_s));
    CHECK(seen.link->instructions == u"Complete authorization in your browser."_s);
    CHECK_FALSE(seen.link->browserOpened);                      // opencode opens nothing: the caller does
    CHECK_FALSE(seen.needsCodeAtLink);
    CHECK_FALSE(signIn.pendingNeedsCode());
    CHECK_FALSE(seen.signedIn);                                 // still waiting for the browser
    CHECK(signIn.busy());

    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    REQUIRE(seen.signedIn);
    CHECK(seen.failures.isEmpty());
    CHECK(seen.signedIn->state == SignInStatus::State::SignedIn);
    CHECK(seen.signedIn->detail == u"OpenAI"_s);                // seen only because the instance was disposed
    CHECK_FALSE(signIn.busy());

    // authorize with the method's index and no `inputs`, a callback without a code, then the refresh.
    const QStringList requests = rig.requests();
    const qsizetype listed = requests.indexOf(u"GET /provider/auth"_s);
    const qsizetype authorized = requests.indexOf(u"POST /provider/openai/oauth/authorize"_s);
    const qsizetype called = requests.indexOf(u"POST /provider/openai/oauth/callback"_s);
    const qsizetype disposed = requests.indexOf(u"POST /instance/dispose"_s);
    const qsizetype checked = requests.lastIndexOf(u"GET /config/providers"_s);
    CHECK(listed >= 0);
    CHECK(listed < authorized);
    CHECK(authorized < called);
    CHECK(called < disposed);
    CHECK(disposed < checked);
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/authorize"_s).first().value(u"body"_s).toObject()
          == QJsonObject{{u"method"_s, 0}});
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/callback"_s).first().value(u"body"_s).toObject()
          == QJsonObject{{u"method"_s, 0}});
}

TEST_CASE("The second oauth entry of a provider is addressed by its own index", "[opencode-signin][facade]") {
    SignInRig rig;
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signIn(u"openai#1"_s);                               // the headless (device code) one
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    CHECK(seen.signedIn.has_value());
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/authorize"_s).first().value(u"body"_s).toObject()
          == QJsonObject{{u"method"_s, 1}});
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/callback"_s).first().value(u"body"_s).toObject()
          == QJsonObject{{u"method"_s, 1}});
}

TEST_CASE("An 'auto' sign-in that fails says why in words, and the next one can start", "[opencode-signin][facade]") {
    SignInRig rig({{u"callback"_s,
                    QJsonObject{{u"fail"_s, QJsonObject{{u"status"_s, 400},
                                                        {u"body"_s, QJsonObject{{u"name"_s, u"ProviderAuthOauthCallbackFailed"_s},
                                                                                {u"data"_s, QJsonObject{}}}}}}}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signIn(u"xai#0"_s);
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    REQUIRE(seen.failures.size() == 1);
    CHECK(seen.link.has_value());
    CHECK_FALSE(seen.signedIn);
    CHECK(seen.failures.first().code == ErrorCode::Unauthorized);
    CHECK(seen.failures.first().message.contains(u"timed out"_s));
    CHECK_FALSE(seen.failures.first().message.contains(u"https"_s));
    CHECK_FALSE(signIn.busy());
    CHECK_FALSE(signIn.pendingNeedsCode());
    CHECK_FALSE(rig.requests().contains(u"POST /instance/dispose"_s));      // nothing was signed in

    seen.clear();
    signIn.checkStatus();                                       // it works again
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    CHECK(seen.status.has_value());
}

TEST_CASE("A 'code' sign-in asks for the code, and sends it only when it is given", "[opencode-signin][facade]") {
    const QJsonObject code{{u"url"_s, u"https://auth.example.test/authorize?state=SECRETSTATE"_s},
                           {u"method"_s, u"code"_s},
                           {u"instructions"_s, u"Open the page and copy the code."_s}};
    SignInRig rig({{u"authorize"_s, QJsonObject{{u"openai/0"_s, code}}},
                   {u"callback"_s, QJsonObject{{u"expectCode"_s, u"CODE-1234"_s}}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signIn(u"openai#0"_s);
    REQUIRE(QTest::qWaitFor([&] { return seen.link.has_value(); }, kWaitMs));
    CHECK(seen.link->instructions.contains(u"Open the page and copy the code."_s));
    CHECK(seen.link->instructions.contains(u"paste it"_s));     // the fact is in the words too
    CHECK(seen.needsCodeAtLink);                                // and in the flag, at the moment of linkReady
    CHECK(signIn.pendingNeedsCode());
    CHECK(signIn.busy());

    QTest::qWait(300);                                          // nothing is called until there is a code
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/callback"_s).isEmpty());
    CHECK_FALSE(seen.anyEnd());

    SECTION("the right code signs in") {
        signIn.submitCode(u"  CODE-1234 \n"_s);                // what a paste brings is trimmed
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.signedIn);
        CHECK(seen.signedIn->detail == u"OpenAI"_s);
        CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/callback"_s).first().value(u"body"_s).toObject()
              == QJsonObject{{u"method"_s, 0}, {u"code"_s, u"CODE-1234"_s}});
        CHECK_FALSE(signIn.pendingNeedsCode());
        CHECK_FALSE(signIn.busy());
    }
    SECTION("a wrong code fails the sign-in without showing the code") {
        signIn.submitCode(u"WRONG-9999"_s);
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.failures.size() == 1);
        CHECK_FALSE(seen.signedIn);
        CHECK(seen.failures.first().code == ErrorCode::Unauthorized);
        CHECK(seen.failures.first().message.contains(u"code"_s));
        CHECK_FALSE(seen.failures.first().message.contains(u"WRONG-9999"_s));
        CHECK_FALSE(signIn.busy());
        CHECK_FALSE(signIn.pendingNeedsCode());
    }
    SECTION("an empty paste is refused and the sign-in keeps waiting") {
        signIn.submitCode(u"   "_s);
        REQUIRE(seen.failures.size() == 1);
        CHECK(seen.failures.first().code == ErrorCode::InvalidArgument);
        CHECK(signIn.busy());
        CHECK(signIn.pendingNeedsCode());
        signIn.submitCode(u"CODE-1234"_s);
        REQUIRE(QTest::qWaitFor([&] { return seen.signedIn.has_value(); }, kWaitMs));
    }
}

TEST_CASE("A code with no sign-in waiting for one is refused", "[opencode-signin][facade]") {
    SignInRig rig;
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.submitCode(u"CODE"_s);
    REQUIRE(seen.failures.size() == 1);
    CHECK(seen.failures.first().code == ErrorCode::InvalidArgument);
    CHECK(rig.requests().isEmpty());
}

TEST_CASE("A null authorize body is a failure, not a sign-in", "[opencode-signin][facade]") {
    SignInRig rig({{u"authorize"_s, QJsonObject{{u"poe/0"_s, QJsonValue::Null}}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signIn(u"poe#0"_s);
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    REQUIRE(seen.failures.size() == 1);
    CHECK(seen.failures.first().code == ErrorCode::NotSupported);
    CHECK_FALSE(seen.link);
    CHECK(rig.requests(u"POST"_s, u"/provider/poe/oauth/callback"_s).isEmpty());   // no callback to nothing
    CHECK_FALSE(signIn.busy());
}

TEST_CASE("An authorize that answers an error says it in words; a garbage reply is a Parse failure", "[opencode-signin][facade]") {
    SECTION("an opencode error") {
        SignInRig rig({{u"authorize"_s,
                        QJsonObject{{u"xai/0"_s, QJsonObject{{u"status"_s, 400},
                                                             {u"body"_s, QJsonObject{{u"name"_s, u"ProviderAuthError"_s},
                                                                                     {u"data"_s, QJsonObject{{u"providerID"_s, u"xai"_s},
                                                                                                             {u"message"_s, u"No luck"_s}}}}}}}}}});
        OpenCodeSignIn signIn(rig.server());
        Seen seen(signIn);
        signIn.signIn(u"xai#0"_s);
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.failures.size() == 1);
        CHECK(seen.failures.first().code == ErrorCode::Unauthorized);
        CHECK(seen.failures.first().message.contains(u"No luck"_s));
    }
    SECTION("a server error that is no JSON, as when a port is taken") {
        SignInRig rig({{u"authorize"_s, QJsonObject{{u"xai/0"_s, QJsonObject{{u"status"_s, 500}, {u"raw"_s, u"EADDRINUSE"_s}}}}}});
        OpenCodeSignIn signIn(rig.server());
        Seen seen(signIn);
        signIn.signIn(u"xai#0"_s);
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.failures.size() == 1);
        CHECK(seen.failures.first().code == ErrorCode::Http);
        CHECK(seen.failures.first().message.contains(u"500"_s));
        CHECK(seen.failures.first().message.contains(u"another program"_s));
    }
    SECTION("a reply with no link") {
        SignInRig rig({{u"authorize"_s, QJsonObject{{u"xai/0"_s, QJsonObject{{u"method"_s, u"auto"_s}, {u"instructions"_s, u"x"_s}}}}}});
        OpenCodeSignIn signIn(rig.server());
        Seen seen(signIn);
        signIn.signIn(u"xai#0"_s);
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.failures.size() == 1);
        CHECK(seen.failures.first().code == ErrorCode::Parse);
        CHECK_FALSE(seen.link);
    }
}

TEST_CASE("Only the offered methods can be signed in to: no questions, no API keys, no unknown ids", "[opencode-signin][facade]") {
    SignInRig rig;
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    const std::vector<std::pair<QString, ErrorCode>> refused{
        {u"github-copilot#0"_s, ErrorCode::NotSupported},       // asks which GitHub first
        {u"openai#2"_s, ErrorCode::NotSupported},               // an API key entry
        {u"openai#9"_s, ErrorCode::NotFound},                   // no such entry
        {u"gitlab#0"_s, ErrorCode::NotSupported},
        {u"cloudflare-workers-ai#0"_s, ErrorCode::InvalidArgument},   // not in the allow-list
        {u"google#0"_s, ErrorCode::InvalidArgument},            // a plugin's
        {u"nonsense"_s, ErrorCode::InvalidArgument},
        {QString(), ErrorCode::InvalidArgument}};               // opencode has many: the caller must choose
    for (const auto &[id, code] : refused) {
        seen.clear();
        signIn.signIn(id);
        REQUIRE(QTest::qWaitFor([&] { return !seen.failures.isEmpty(); }, kWaitMs));
        INFO(id.toStdString());
        CHECK(seen.failures.first().code == code);
        CHECK_FALSE(seen.link);
        CHECK_FALSE(signIn.busy());
    }
    for (const QJsonObject &entry : rig.log()) {
        CHECK_FALSE(entry.value(u"path"_s).toString().contains(u"/oauth/"_s));  // not one authorize or callback
    }
}

TEST_CASE("Cancel stops waiting for the browser, ends the held request and leaves the next sign-in free", "[opencode-signin][facade]") {
    SignInRig rig({{u"callback"_s, QJsonObject{{u"delay"_s, 60}}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signIn(u"openai#0"_s);
    REQUIRE(QTest::qWaitFor([&] { return seen.link.has_value(); }, kWaitMs));
    REQUIRE(QTest::qWaitFor([&] { return !rig.requests(u"POST"_s, u"/provider/openai/oauth/callback"_s).isEmpty(); },
                            kWaitMs));
    CHECK(signIn.busy());

    signIn.cancel();
    REQUIRE(seen.failures.size() == 1);                         // at once, not when the server answers
    CHECK(seen.failures.first().code == ErrorCode::Cancelled);
    CHECK_FALSE(signIn.busy());
    CHECK_FALSE(signIn.pendingNeedsCode());
    QTest::qWait(300);
    CHECK(seen.failures.size() == 1);                           // and the aborted reply says nothing more
    CHECK_FALSE(seen.signedIn);

    seen.clear();
    signIn.checkStatus();
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    CHECK(seen.status.has_value());
    signIn.cancel();                                            // nothing running: nothing said
    CHECK(seen.failures.isEmpty());
}

TEST_CASE("Cancel while the server is still starting fails the sign-in once, whatever the server then does", "[opencode-signin][facade]") {
    SignInRig rig;
    std::shared_ptr<OpenCodeServer> server = rig.server();
    OpenCodeSignIn signIn(server);                              // the real start takes a moment
    Seen seen(signIn);
    signIn.signIn(u"openai#0"_s);
    CHECK(signIn.busy());
    CHECK_FALSE(server->isReady());
    signIn.cancel();
    REQUIRE(seen.failures.size() == 1);
    CHECK(seen.failures.first().code == ErrorCode::Cancelled);
    REQUIRE(QTest::qWaitFor([&] { return server->isReady(); }, kWaitMs));     // the server comes up; nothing follows
    QTest::qWait(500);
    CHECK(seen.failures.size() == 1);
    CHECK_FALSE(seen.link);
    CHECK_FALSE(signIn.busy());
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/authorize"_s).isEmpty());
}

TEST_CASE("One step at a time: a second request while busy is refused and the first goes on", "[opencode-signin][facade]") {
    SignInRig rig({{u"callback"_s, QJsonObject{{u"delay"_s, 0.6}}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signIn(u"openai#0"_s);
    REQUIRE(QTest::qWaitFor([&] { return seen.link.has_value(); }, kWaitMs));
    signIn.checkStatus();
    signIn.signIn(u"xai#0"_s);
    signIn.listMethods();
    signIn.signOut();
    REQUIRE(seen.failures.size() == 4);
    for (const Error &error : seen.failures) CHECK(error.code == ErrorCode::InvalidArgument);
    CHECK_FALSE(seen.status);
    CHECK_FALSE(seen.methods);
    REQUIRE(QTest::qWaitFor([&] { return seen.signedIn.has_value(); }, kWaitMs));
    CHECK(seen.failures.size() == 4);
    CHECK(rig.requests(u"POST"_s, u"/provider/xai/oauth/authorize"_s).isEmpty());
}

TEST_CASE("Sign out removes the stored login of every offered provider that is connected, and nothing else", "[opencode-signin][facade]") {
    SignInRig rig({{u"connected"_s, QJsonArray{u"github-copilot"_s, u"xai"_s, u"google"_s, u"opencode"_s}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signOut();
    CHECK(signIn.busy());
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    CHECK(seen.failures.isEmpty());
    CHECK(seen.signedOut == 1);
    CHECK(rig.requests(u"DELETE"_s, u"/auth/github-copilot"_s).size() == 1);
    CHECK(rig.requests(u"DELETE"_s, u"/auth/xai"_s).size() == 1);
    CHECK(rig.requests(u"DELETE"_s, u"/auth/google"_s).isEmpty());      // a plugin's: not Nexa's to remove
    CHECK(rig.requests(u"DELETE"_s, u"/auth/opencode"_s).isEmpty());
    CHECK(rig.requests().contains(u"POST /instance/dispose"_s));
    CHECK_FALSE(signIn.busy());

    seen.clear();
    signIn.checkStatus();
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    CHECK(seen.status->state == SignInStatus::State::SignedOut);
}

TEST_CASE("Signing out when nothing is signed in removes nothing", "[opencode-signin][facade]") {
    SignInRig rig({{u"connected"_s, QJsonArray{u"opencode"_s}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signOut();
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    CHECK(seen.signedOut == 1);
    for (const QJsonObject &entry : rig.log()) CHECK(entry.value(u"method"_s).toString() != u"DELETE"_s);
}

TEST_CASE("A login that stays after the removal is reported, not called signed out", "[opencode-signin][facade]") {
    // As when OPENAI_API_KEY is set in the environment: DELETE /auth/<id> cannot remove that.
    SignInRig rig({{u"connected"_s, QJsonArray{u"openai"_s}}, {u"sticky"_s, QJsonArray{u"openai"_s}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signOut();
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    CHECK(seen.signedOut == 0);
    REQUIRE(seen.failures.size() == 1);
    CHECK(seen.failures.first().message.contains(u"OpenAI"_s));
    CHECK(seen.failures.first().message.contains(u"environment"_s));
    CHECK_FALSE(signIn.busy());
}

TEST_CASE("No code, link or token reaches the log, whatever the sign-in does", "[opencode-signin][facade]") {
    LogCapture capture;
    {
        const QJsonObject code{{u"url"_s, u"https://auth.example.test/authorize?state=SECRETSTATE&code_challenge=SECRETCHALLENGE"_s},
                               {u"method"_s, u"code"_s}, {u"instructions"_s, u"Paste SECRETDEVICE here."_s}};
        SignInRig rig({{u"authorize"_s, QJsonObject{{u"openai/0"_s, code}}},
                       {u"callback"_s, QJsonObject{{u"expectCode"_s, u"NOTTHISONE"_s}}}});
        OpenCodeSignIn signIn(rig.server());
        Seen seen(signIn);
        signIn.signIn(u"openai#0"_s);
        REQUIRE(QTest::qWaitFor([&] { return seen.link.has_value(); }, kWaitMs));
        signIn.submitCode(u"SECRETCODE-777"_s);                // refused by the fake: a failure with a code in play
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        CHECK(seen.failures.size() == 1);
    }
    const QString everything = LogCapture::lines().join(u'\n');
    CHECK_FALSE(everything.contains(u"SECRETSTATE"_s));
    CHECK_FALSE(everything.contains(u"SECRETCHALLENGE"_s));
    CHECK_FALSE(everything.contains(u"SECRETDEVICE"_s));
    CHECK_FALSE(everything.contains(u"SECRETCODE"_s));
    CHECK_FALSE(everything.contains(u"auth.example.test"_s));
    CHECK(everything.contains(u"sign-in"_s));                   // the capture works: the sign-in does log
}

TEST_CASE("The refresh after a sign-in or a sign-out leaves the user's MCP servers out of Nexa's opencode", "[opencode-signin][facade]") {
    // POST /instance/dispose makes opencode build its instance anew, which connects the MCP servers of the user's
    // configuration again — the ones OpenCodeServer disconnected at the start because opencode offers their
    // resources to the model whatever a prompt's tools say. (The fake does the same on a dispose.)
    const auto mcpStatus = [](SignInRig &rig, const std::shared_ptr<OpenCodeServer> &server) {
        QString status = u"unanswered"_s;
        rig.http.get(server->request(u"/mcp"_s), 5'000, nullptr, [&status](Result<QByteArray> body) {
            status = body ? object(body.value()).value(u"fakemcp"_s).toObject().value(u"status"_s).toString()
                          : u"error"_s;
        });
        static_cast<void>(QTest::qWaitFor([&] { return status != u"unanswered"; }, kWaitMs));
        return status;
    };
    SECTION("after a sign-in") {
        SignInRig rig;
        std::shared_ptr<OpenCodeServer> server = rig.server();
        OpenCodeSignIn signIn(server);
        Seen seen(signIn);
        signIn.signIn(u"openai#0"_s);
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.signedIn);
        REQUIRE(rig.requests().contains(u"POST /instance/dispose"_s));
        CHECK(mcpStatus(rig, server) == u"disabled"_s);
    }
    SECTION("after a sign-out") {
        SignInRig rig({{u"connected"_s, QJsonArray{u"xai"_s}}});
        std::shared_ptr<OpenCodeServer> server = rig.server();
        OpenCodeSignIn signIn(server);
        Seen seen(signIn);
        signIn.signOut();
        REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
        REQUIRE(seen.signedOut == 1);
        REQUIRE(rig.requests().contains(u"POST /instance/dispose"_s));
        CHECK(mcpStatus(rig, server) == u"disabled"_s);
    }
}

TEST_CASE("After a cancel the same sign-in starts again: a new authorize replaces the one opencode still holds", "[opencode-signin][facade]") {
    const QJsonObject code{{u"url"_s, u"https://auth.example.test/authorize?state=S"_s},
                           {u"method"_s, u"code"_s},
                           {u"instructions"_s, u"Copy the code."_s}};
    SignInRig rig({{u"authorize"_s, QJsonObject{{u"openai/0"_s, code}}},
                   {u"callback"_s, QJsonObject{{u"expectCode"_s, u"CODE-1"_s}}}});
    OpenCodeSignIn signIn(rig.server());
    Seen seen(signIn);
    signIn.signIn(u"openai#0"_s);
    REQUIRE(QTest::qWaitFor([&] { return seen.link.has_value(); }, kWaitMs));
    signIn.cancel();                                            // opencode keeps its side of the flow
    REQUIRE(seen.failures.size() == 1);
    CHECK_FALSE(signIn.pendingNeedsCode());
    signIn.submitCode(u"CODE-1"_s);                             // the cancelled flow takes no code any more
    REQUIRE(seen.failures.size() == 2);
    CHECK(seen.failures.last().code == ErrorCode::InvalidArgument);
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/callback"_s).isEmpty());

    seen.clear();
    signIn.signIn(u"openai#0"_s);
    REQUIRE(QTest::qWaitFor([&] { return seen.link.has_value(); }, kWaitMs));
    CHECK(signIn.pendingNeedsCode());
    signIn.submitCode(u"CODE-1"_s);
    REQUIRE(QTest::qWaitFor([&] { return seen.anyEnd(); }, kWaitMs));
    CHECK(seen.signedIn.has_value());
    CHECK(seen.failures.isEmpty());
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/authorize"_s).size() == 2);
    CHECK(rig.requests(u"POST"_s, u"/provider/openai/oauth/callback"_s).size() == 1);
}
