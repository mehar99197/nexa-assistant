#include "app/AppController.h"
#include "app/RemoteAccess.h"
#include "app/SettingsDialog.h"
#include "core/account/AccountManager.h"
#include "core/account/CredentialStore.h"
#include "core/agent/AppContext.h"
#include "core/ai/ProviderRegistry.h"
#include "core/audit/AuditLog.h"
#include "core/config/ConfigStore.h"
#include "core/intent/IntentRouter.h"
#include "core/net/HttpClient.h"
#include "core/policy/PanicController.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"
#include "remote/IRemoteHost.h"
#include "remote/LocalApiServer.h"
#include "remote/PairingManager.h"
#include "remote/RemoteServer.h"
#include "remote/TlsIdentity.h"

#include "FakeInputLock.h"
#include "LicenseTestUtil.h"
#include "QtStringMakers.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUrl>

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 5'000;

struct TestHost final : IRemoteHost {
    bool isBusy() const override { return false; }
    void submit(const QString &, const QString &) override {}
    void cancel() override {}
    void transcribe(const QByteArray &, std::function<void(Result<QString>)> done) override { done(QString()); }
    bool canTranscribe() const override { return false; }
    void renderSpeech(const QString &, std::function<void(Result<QByteArray>)> done) override { done(QByteArray()); }
};

class RelayMockServer {
public:
    struct Request {
        QByteArray method;
        QString path;
        QByteArray authorization;
        QByteArray body;
    };

    struct Reply {
        int status{200};
        QByteArray contentType{"application/json"};
        QList<QPair<QByteArray, QByteArray>> extraHeaders;
        QByteArray body;
    };

    using Handler = std::function<Reply(const Request &)>;

    explicit RelayMockServer(Handler handler) : handler_(std::move(handler)) {
        REQUIRE(server_.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            while (QTcpSocket *sock = server_.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
                    buf->append(sock->readAll());
                    const qsizetype hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray head = buf->left(hdrEnd);
                    qsizetype len = 0;
                    QByteArray auth;
                    for (const QByteArray &line : head.split('\n')) {
                        const qsizetype c = line.indexOf(':');
                        if (c <= 0) continue;
                        if (line.left(c).trimmed().toLower() == "content-length") {
                            len = line.mid(c + 1).trimmed().toLongLong();
                        } else if (line.left(c).trimmed().toLower() == "authorization") {
                            auth = line.mid(c + 1).trimmed();
                        }
                    }
                    if (buf->size() < hdrEnd + 4 + len) return;
                    const QList<QByteArray> parts = head.split('\n').first().trimmed().split(' ');
                    Request req{parts.value(0), QString::fromUtf8(parts.value(1)), auth, buf->mid(hdrEnd + 4, len)};
                    seen.append(req);
                    const Reply res = handler_(req);
                    QByteArray wire = "HTTP/1.1 " + QByteArray::number(res.status) + " OK\r\n"
                                    + "Content-Type: " + res.contentType + "\r\n"
                                    + "Content-Length: " + QByteArray::number(res.body.size()) + "\r\n";
                    for (const auto &hdr : res.extraHeaders) {
                        wire += hdr.first + ": " + hdr.second + "\r\n";
                    }
                    wire += "Connection: close\r\n\r\n" + res.body;
                    sock->write(wire);
                    sock->flush();
                    sock->disconnectFromHost();
                });
            }
        });
    }

    QString rootUrl() const { return u"http://127.0.0.1:%1"_s.arg(server_.serverPort()); }
    QList<Request> seen;

private:
    QTcpServer server_;
    Handler handler_;
};

struct HttpPostResult {
    int status{0};
    QByteArray body;
};

HttpPostResult syncPost(quint16 port, const QString &path, const QByteArray &token, const QByteArray &body) {
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(u"http://127.0.0.1:%1%2"_s.arg(port).arg(path)));
    req.setRawHeader("Authorization", "Bearer " + token);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    QNetworkReply *reply = nam.post(req, body);
    QSignalSpy done(reply, &QNetworkReply::finished);
    REQUIRE(done.wait(3'000));
    HttpPostResult res{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll()};
    reply->deleteLater();
    return res;
}

} // namespace

TEST_CASE("AccountIntegration: Free tier fails closed across AppController, RemoteAccess, LocalApi, and SettingsDialog",
          "[account][integration]") {
    credentialstore::clearForTesting();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QByteArray oldUrl = qgetenv("NEXA_LICENSE_API_URL");
    const QByteArray oldInsecure = qgetenv("NEXA_ALLOW_INSECURE_LICENSE_API");
    qunsetenv("NEXA_LICENSE_API_URL");
    qunsetenv("NEXA_ALLOW_INSECURE_LICENSE_API");
    const auto restore = qScopeGuard([&oldUrl, &oldInsecure] {
        credentialstore::clearForTesting();
        if (oldUrl.isEmpty()) qunsetenv("NEXA_LICENSE_API_URL");
        else qputenv("NEXA_LICENSE_API_URL", oldUrl);
        if (oldInsecure.isEmpty()) qunsetenv("NEXA_ALLOW_INSECURE_LICENSE_API");
        else qputenv("NEXA_ALLOW_INSECURE_LICENSE_API", oldInsecure);
    });

    AccountManager account(dir.filePath(u"account.ini"_s));
    REQUIRE_FALSE(account.verifiedFeatures().nexaAi);
    REQUIRE_FALSE(account.verifiedFeatures().ownVoice);
    REQUIRE_FALSE(account.verifiedFeatures().awayFromHome);

    RelayMockServer relay([](const RelayMockServer::Request &) {
        return RelayMockServer::Reply{500, "application/json", {}, "{}"};
    });

    ConfigStore store(dir.filePath(u"config.json"_s));
    HttpClient http;
    ProviderRegistry registry(&http);
    ToolRegistry tools;
    PolicyEngine policy;
    AuditLog audit(dir.filePath(u"audit.jsonl"_s));
    ToolExecutor executor(&tools, &policy, &audit);
    PanicController panic;
    IntentRouter router([&tools](const QString &t) { return tools.contains(t); });
    test::FakeLock lock;

    Config config;
    ProviderConfig nexaAi = ProviderRegistry::fromPreset(ProviderRegistry::nexaAiPreset());
    nexaAi.baseUrl = relay.rootUrl() + u"/api/assistant/v1"_s;
    config.providers = {nexaAi};
    config.activeProvider = u"nexa-ai"_s;
    config.voice.ownVoice = true;

    AppController controller(
        AppServices{.store = &store,
                    .providers = &registry,
                    .tools = &tools,
                    .executor = &executor,
                    .router = &router,
                    .panic = &panic,
                    .policy = &policy,
                    .http = &http,
                    .inputLock = &lock,
                    .account = &account},
        config, u"linux-test"_s);

    QSignalSpy required(&controller, &AppController::accountRequired);
    controller.start();
    CHECK(required.count() >= 1);
    CHECK(controller.submit(u"explain quicksort"_s) == 0);
    QTest::qWait(50);
    CHECK(relay.seen.isEmpty()); // Free tier must not even send requests to relay
    CHECK_FALSE(controller.runtimeConfig().voice.ownVoice);

    // RemoteAccess away-from-home tunnels gated
    PairingManager pairing(dir.filePath(u"paired.json"_s));
    TlsIdentity tls(dir.filePath(u"tls"_s));
    TestHost host;
    RemoteServer remote(&pairing, &host, &tls);
    RemoteAccess remoteAccess(&remote);
    remoteAccess.setEntitled(account.verifiedFeatures().awayFromHome);
    CHECK_FALSE(remoteAccess.isEntitled());
    CHECK(remoteAccess.describe(RemoteRoute::Kind::Cloudflare).contains(u"Pro or Team"_s));

    // LocalApiServer refuses managed Nexa AI on Free plan
    LocalApiServer localApi(&registry);
    localApi.setConfigTransform([&account](const Config &cfg) -> Result<Config> {
        Config copy = cfg;
        const ProviderConfig *active = copy.active();
        if (active && ProviderRegistry::isNexaManaged(*active) && !account.verifiedFeatures().nexaAi) {
            return Error{ErrorCode::Unauthorized, u"Nexa AI requires a Pro or Team plan."_s};
        }
        return copy;
    });
    config.localApi.enabled = true;
    config.localApi.port = 0;
    config.localApi.bearerToken = u"tok-free-test"_s;
    REQUIRE(localApi.start(config));
    const HttpPostResult localResp =
        syncPost(localApi.port(), u"/v1/chat/completions"_s, "tok-free-test",
                 R"({"model":"nexa-fast","messages":[{"role":"user","content":"hi"}]})");
    CHECK(localResp.status == 503);
    CHECK(localResp.body.contains("Pro or Team"));

    // SettingsDialog: Account tab present, Own Voice & Away From Home disabled, Local API saved cleanly
    SettingsDialog dialog(config, &registry, &http,
                          SystemStatus{.remote = &remote, .remoteAccess = &remoteAccess, .account = &account});
    auto *tabs = dialog.findChild<QTabWidget *>();
    REQUIRE(tabs != nullptr);
    QStringList tabTitles;
    for (int i = 0; i < tabs->count(); ++i) tabTitles.append(tabs->tabText(i));
    CHECK(tabTitles.contains(u"Account"_s));

    auto *ownVoice = dialog.findChild<QCheckBox *>(u"ownVoice"_s);
    auto *recordBtn = dialog.findChild<QPushButton *>(u"recordSample"_s);
    auto *tailscaleCheck = dialog.findChild<QCheckBox *>(u"tailscaleCheck"_s);
    auto *cloudflareMode = dialog.findChild<QComboBox *>(u"cloudflareMode"_s);
    auto *ngrokCheck = dialog.findChild<QCheckBox *>(u"ngrokCheck"_s);
    REQUIRE(ownVoice != nullptr);
    REQUIRE(recordBtn != nullptr);
    REQUIRE(tailscaleCheck != nullptr);
    REQUIRE(cloudflareMode != nullptr);
    REQUIRE(ngrokCheck != nullptr);
    CHECK_FALSE(ownVoice->isEnabled());
    CHECK_FALSE(recordBtn->isEnabled());
    CHECK_FALSE(tailscaleCheck->isEnabled());
    CHECK_FALSE(cloudflareMode->isEnabled());
    CHECK_FALSE(ngrokCheck->isEnabled());

    // Configure Local API in Account tab and accept dialog
    auto *localApiEnabled = dialog.findChild<QCheckBox *>(u"localApiEnabled"_s);
    auto *localApiPort = dialog.findChild<QSpinBox *>(u"localApiPort"_s);
    auto *localApiToken = dialog.findChild<QLineEdit *>(u"localApiToken"_s);
    REQUIRE(localApiEnabled != nullptr);
    REQUIRE(localApiPort != nullptr);
    REQUIRE(localApiToken != nullptr);
    localApiEnabled->setChecked(true);
    localApiPort->setValue(18999);
    localApiToken->clear();
    dialog.accept();
    CHECK(dialog.config().localApi.enabled);
    CHECK(dialog.config().localApi.port == 18999);
    CHECK_FALSE(dialog.config().localApi.bearerToken.isEmpty());
}

TEST_CASE("AccountIntegration: Pro token injects runtime bearer without persisting apiKey and tracks X-Nexa-Remaining",
          "[account][integration]") {
    credentialstore::clearForTesting();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString settingsPath = dir.filePath(u"account.ini"_s);
    const QString fp = AccountManager::deviceFingerprint(settingsPath);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString signedJwt = test::signAssistantJwt(
        u"pro"_s, fp, now - 60, now + 3600,
        QJsonObject{{u"nexaAi"_s, true}, {u"ownVoice"_s, true}, {u"awayFromHome"_s, true}, {u"seats"_s, 1}});

    QString validatedProduct;
    QByteArray seenAuthHeader;
    RelayMockServer server([&](const RelayMockServer::Request &req) -> RelayMockServer::Reply {
        if (req.method == "POST" && req.path == "/api/license/validate") {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            validatedProduct = body.value(u"product"_s).toString();
            const QJsonObject resp{
                {u"valid"_s, true},
                {u"plan"_s, u"pro"_s},
                {u"seats"_s, 1},
                {u"activeSeats"_s, 1},
                {u"expires"_s, QDateTime::fromSecsSinceEpoch(now + 3600, QTimeZone::utc()).toString(Qt::ISODate)},
                {u"token"_s, signedJwt},
                {u"features"_s, QJsonObject{{u"nexaAi"_s, true}, {u"ownVoice"_s, true}, {u"awayFromHome"_s, true}}},
            };
            return {200, "application/json", {}, QJsonDocument(resp).toJson(QJsonDocument::Compact)};
        }
        if (req.method == "GET" && req.path == "/api/assistant/v1/models") {
            seenAuthHeader = req.authorization;
            return {200, "application/json", {}, R"({"data":[{"id":"nexa-fast"}]})"};
        }
        if (req.method == "POST" && req.path == "/api/assistant/v1/chat/completions") {
            seenAuthHeader = req.authorization;
            const QByteArray sse =
                "data: {\"choices\":[{\"delta\":{\"content\":\"Pro answer\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n";
            return {200, "text/event-stream", {{"X-Nexa-Remaining", "941"}}, sse};
        }
        if (req.method == "POST" && req.path == "/api/license/deactivate") {
            return {200, "application/json", {}, R"({"ok":true})"};
        }
        return {404, "application/json", {}, R"({"error":"not found"})"};
    });

    const QByteArray oldUrl = qgetenv("NEXA_LICENSE_API_URL");
    const QByteArray oldInsecure = qgetenv("NEXA_ALLOW_INSECURE_LICENSE_API");
    qputenv("NEXA_LICENSE_API_URL", (server.rootUrl() + u"/api/license/validate"_s).toUtf8());
    qputenv("NEXA_ALLOW_INSECURE_LICENSE_API", "1");
    const auto restore = qScopeGuard([&oldUrl, &oldInsecure] {
        credentialstore::clearForTesting();
        if (oldUrl.isEmpty()) qunsetenv("NEXA_LICENSE_API_URL");
        else qputenv("NEXA_LICENSE_API_URL", oldUrl);
        if (oldInsecure.isEmpty()) qunsetenv("NEXA_ALLOW_INSECURE_LICENSE_API");
        else qputenv("NEXA_ALLOW_INSECURE_LICENSE_API", oldInsecure);
    });

    AccountManager account(settingsPath);
    QSignalSpy activated(&account, &AccountManager::activationFinished);
    account.activate(u"NDM-AAAA-BBBB-CCCC"_s);
    REQUIRE(activated.wait(kWaitMs));
    INFO("activation message: " << activated.last().at(1).toString().toStdString());
    REQUIRE(activated.last().at(0).toBool());
    REQUIRE(account.verifiedFeatures().nexaAi);
    CHECK(validatedProduct == u"assistant"_s);
    CHECK(account.verifiedFeatures().ownVoice);
    CHECK(account.verifiedFeatures().awayFromHome);
    CHECK(account.licenseToken() == signedJwt);

    HttpClient http;
    QObject::connect(&http, &HttpClient::nexaRemaining, &account, &AccountManager::noteUsageRemaining);
    ConfigStore store(dir.filePath(u"config.json"_s));
    ProviderRegistry registry(&http);
    ToolRegistry tools;
    PolicyEngine policy;
    AuditLog audit(dir.filePath(u"audit.jsonl"_s));
    ToolExecutor executor(&tools, &policy, &audit);
    PanicController panic;
    IntentRouter router([&tools](const QString &t) { return tools.contains(t); });
    test::FakeLock lock;

    Config config;
    ProviderConfig nexaAi = ProviderRegistry::fromPreset(ProviderRegistry::nexaAiPreset());
    config.providers = {nexaAi};
    config.activeProvider = u"nexa-ai"_s;
    config.voice.ownVoice = true;
    REQUIRE(store.save(config).isOk());

    AppController controller(
        AppServices{.store = &store,
                    .providers = &registry,
                    .tools = &tools,
                    .executor = &executor,
                    .router = &router,
                    .panic = &panic,
                    .policy = &policy,
                    .http = &http,
                    .inputLock = &lock,
                    .account = &account},
        config, u"linux-test"_s);

    controller.start();
    REQUIRE(QTest::qWaitFor([&seenAuthHeader] { return !seenAuthHeader.isEmpty(); }, kWaitMs));
    CHECK(seenAuthHeader == ("Bearer " + signedJwt.toUtf8()));
    CHECK(controller.runtimeConfig().voice.ownVoice);

    seenAuthHeader.clear();
    const quint64 taskId = controller.submit(u"say hello"_s);
    REQUIRE(taskId != 0);
    REQUIRE(QTest::qWaitFor(
        [&controller, taskId] { return controller.mission()->task(taskId)->state == TaskState::Done; }, kWaitMs));
    CHECK(seenAuthHeader == ("Bearer " + signedJwt.toUtf8()));
    CHECK(account.aiUsage().known);
    CHECK(account.aiUsage().remaining == 941);

    // Crucial security invariant: config.json on disk must NEVER store the signed JWT as apiKey.
    QFile diskConfig(dir.filePath(u"config.json"_s));
    REQUIRE(diskConfig.open(QIODevice::ReadOnly));
    const QByteArray diskBytes = diskConfig.readAll();
    CHECK_FALSE(diskBytes.contains(signedJwt.toUtf8()));

    // LocalApiServer injects runtime token when serving managed Nexa AI on Pro plan
    LocalApiServer localApi(&registry);
    localApi.setConfigTransform([&account](const Config &cfg) -> Result<Config> {
        Config copy = cfg;
        for (ProviderConfig &p : copy.providers) {
            if (ProviderRegistry::isNexaManaged(p)) {
                p.apiKey = account.licenseToken();
                p.baseUrl = account.nexaAiBaseUrl();
            }
        }
        return copy;
    });
    config.localApi.enabled = true;
    config.localApi.port = 0;
    config.localApi.bearerToken = u"tok-pro-test"_s;
    REQUIRE(localApi.start(config));

    seenAuthHeader.clear();
    const HttpPostResult localResp =
        syncPost(localApi.port(), u"/v1/chat/completions"_s, "tok-pro-test",
                 R"({"model":"nexa-fast","messages":[{"role":"user","content":"via proxy"}]})");
    CHECK(localResp.status == 200);
    CHECK(localResp.body.contains("Pro answer"));
    CHECK(seenAuthHeader == ("Bearer " + signedJwt.toUtf8()));

    account.releaseSeat();
}
