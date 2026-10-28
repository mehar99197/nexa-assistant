// core/ai/ProviderChain: the next provider when one cannot answer, with scripted providers as links.

#include "core/ai/ChatStream.h"
#include "core/ai/ProviderChain.h"
#include "core/ai/ProviderRegistry.h"
#include "core/net/HttpClient.h"

#include "QtStringMakers.h"

#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 5'000;

class FakeStream : public ChatStream {
public:
    void play(const QString &text, std::optional<Error> error) {
        QTimer::singleShot(0, this, [this, text, error] {
            emitText(text);
            if (error) {
                fail(*error);
            } else {
                complete(FinishReason::Stop, {});
            }
        });
    }

protected:
    void abortTransport() override {}
};

// A provider whose replies are scripted: text, then an error or the end.
class FakeLink : public IAiProvider {
public:
    struct Outcome {
        QString text;
        std::optional<Error> error;
    };

    explicit FakeLink(QString name, bool native = true) : name_(std::move(name)), native_(native) {}

    QString id() const override { return name_.toLower(); }
    QString displayName() const override { return name_; }
    Capabilities capabilities() const override { return {.nativeTools = native_}; }
    void listModels(std::function<void(Result<QStringList>)> done) override { done(QStringList{name_ + u"-model"_s}); }
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override {
        requests.append(request);
        const Outcome outcome = outcomes.isEmpty() ? Outcome{u"from "_s + name_, std::nullopt} : outcomes.takeFirst();
        auto *stream = new FakeStream;
        stream->play(outcome.text, outcome.error);
        return std::shared_ptr<ChatStream>(stream, [](ChatStream *done) { done->deleteLater(); });
    }
    void warmUp() override { ++warmed; }

    QList<Outcome> outcomes;
    QList<ChatRequest> requests;
    int warmed{0};

private:
    QString name_;
    bool native_;
};

const Error kDown{ErrorCode::ConnectionRefused, u"down"_s};

struct Chain {
    Chain(std::initializer_list<std::pair<FakeLink *, QString>> links) {
        std::vector<ProviderChain::Link> owned;
        for (const auto &[link, model] : links) {
            fakes.append(link);
            owned.push_back({std::unique_ptr<IAiProvider>(link), model});
        }
        chain = std::make_unique<ProviderChain>(std::move(owned));
    }

    struct Outcome {
        QString text;
        std::optional<FinishReason> finish;
        std::optional<Error> error;
        QStringList reroutes;     // "route | reason"
        QString route;
    };

    Outcome ask(ChatRequest request = {.model = u"m1"_s, .messages = {{.role = Role::User, .text = u"hi"_s}}}) {
        Outcome outcome;
        std::shared_ptr<ChatStream> stream = chain->chat(request);
        QObject::connect(stream.get(), &ChatStream::textDelta, [&](const QString &delta) { outcome.text += delta; });
        QObject::connect(stream.get(), &ChatStream::finished, [&](FinishReason reason) { outcome.finish = reason; });
        QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &error) { outcome.error = error; });
        QObject::connect(stream.get(), &ChatStream::rerouted, [&](const QString &route, const QString &reason) {
            outcome.reroutes.append(route + u" | "_s + reason);
        });
        REQUIRE(QTest::qWaitFor([&] { return outcome.finish || outcome.error; }, kWaitMs));
        outcome.route = stream->route();
        return outcome;
    }

    QList<FakeLink *> fakes;
    std::unique_ptr<ProviderChain> chain;
};

} // namespace

TEST_CASE("The first link answers when it can") {
    Chain c{{new FakeLink(u"One"_s), u"m1"_s}, {new FakeLink(u"Two"_s), u"m2"_s}};
    const Chain::Outcome outcome = c.ask();
    CHECK(outcome.text == u"from One"_s);
    CHECK(outcome.route.isEmpty());
    CHECK(outcome.reroutes.isEmpty());
    CHECK(c.fakes[1]->requests.isEmpty());
    CHECK(c.chain->displayName() == u"One"_s);
    c.chain->warmUp();
    CHECK(c.fakes[0]->warmed == 1);
    CHECK(c.fakes[1]->warmed == 0);
}

TEST_CASE("An unreachable first link hands the request to the next, which uses its own model") {
    Chain c{{new FakeLink(u"One"_s), u"m1"_s}, {new FakeLink(u"Two"_s), u"m2"_s}};
    c.fakes[0]->outcomes = {{{}, kDown}};
    const Chain::Outcome outcome = c.ask();
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.text == u"from Two"_s);
    REQUIRE(c.fakes[1]->requests.size() == 1);
    CHECK(c.fakes[1]->requests.first().model == u"m2"_s);
    REQUIRE(outcome.reroutes.size() == 1);
    CHECK(outcome.reroutes.first().startsWith(u"Two · m2 | One: "_s));
    CHECK(outcome.route == u"Two · m2"_s);
}

TEST_CASE("A failure after text has streamed is not retried elsewhere") {
    Chain c{{new FakeLink(u"One"_s), u"m1"_s}, {new FakeLink(u"Two"_s), u"m2"_s}};
    c.fakes[0]->outcomes = {{u"partial"_s, kDown}};
    const Chain::Outcome outcome = c.ask();
    REQUIRE(outcome.error);
    CHECK(outcome.text == u"partial"_s);
    CHECK(c.fakes[1]->requests.isEmpty());
}

TEST_CASE("A 400 refusal is passed through, a 429 moves on") {
    Chain refused{{new FakeLink(u"One"_s), u"m1"_s}, {new FakeLink(u"Two"_s), u"m2"_s}};
    refused.fakes[0]->outcomes = {{{}, Error{ErrorCode::Http, u"HTTP 400: image"_s}}};
    const Chain::Outcome first = refused.ask();
    REQUIRE(first.error);
    CHECK(first.error->message == u"HTTP 400: image"_s);
    CHECK(refused.fakes[1]->requests.isEmpty());

    Chain busy{{new FakeLink(u"One"_s), u"m1"_s}, {new FakeLink(u"Two"_s), u"m2"_s}};
    busy.fakes[0]->outcomes = {{{}, Error{ErrorCode::Http, u"HTTP 429: busy"_s}}};
    CHECK(busy.ask().text == u"from Two"_s);

    CHECK(ProviderChain::meansUnavailable(Error{ErrorCode::Http, u"[503]: overloaded"_s}));
    CHECK(ProviderChain::meansUnavailable(Error{ErrorCode::Unauthorized, u"sign in"_s}));
    CHECK(ProviderChain::meansUnavailable(Error{ErrorCode::NotFound, u"not installed"_s}));
    CHECK_FALSE(ProviderChain::meansUnavailable(Error{ErrorCode::Http, u"HTTP 404: no such model"_s}));
    CHECK_FALSE(ProviderChain::meansUnavailable(Error{ErrorCode::Cancelled, u"Cancelled"_s}));
}

TEST_CASE("A link that failed is skipped for the window, then tried again") {
    Chain c{{new FakeLink(u"One"_s), u"m1"_s}, {new FakeLink(u"Two"_s), u"m2"_s}};
    c.chain->setSkipWindowMs(150);
    c.fakes[0]->outcomes = {{{}, kDown}, {u"back"_s, std::nullopt}};
    CHECK(c.ask().text == u"from Two"_s);
    CHECK(c.ask().text == u"from Two"_s);
    CHECK(c.fakes[0]->requests.size() == 1);        // skipped the second time
    QTest::qWait(250);
    CHECK(c.ask().text == u"back"_s);
}

TEST_CASE("When every link fails, one message names each") {
    Chain c{{new FakeLink(u"One"_s), u"m1"_s}, {new FakeLink(u"Two"_s), u"m2"_s}};
    c.fakes[0]->outcomes = {{{}, kDown}};
    c.fakes[1]->outcomes = {{{}, Error{ErrorCode::NotFound, u"missing"_s}}};
    const Chain::Outcome outcome = c.ask();
    REQUIRE(outcome.error);
    CHECK(outcome.error->message.contains(u"One: down"_s));
    CHECK(outcome.error->message.contains(u"; Two: missing"_s));
}

TEST_CASE("A native request falling to a text-only link is converted") {
    Chain c{{new FakeLink(u"One"_s), u"m1"_s}, {new FakeLink(u"Two"_s, false), u"m2"_s}};
    CHECK(c.chain->capabilities().nativeTools);
    c.fakes[0]->outcomes = {{{}, kDown}};
    const ChatRequest request{.model = u"m1"_s,
                              .messages = {{.role = Role::System, .text = u"S"_s}, {.role = Role::User, .text = u"hi"_s}},
                              .tools = {ToolSpec{u"echo"_s, u"Echo"_s, {}}}};
    REQUIRE_FALSE(c.ask(request).error);
    REQUIRE(c.fakes[1]->requests.size() == 1);
    const ChatRequest &converted = c.fakes[1]->requests.first();
    CHECK(converted.tools.isEmpty());
    REQUIRE(converted.messages.first().role == Role::System);
    CHECK(converted.messages.first().text.startsWith(u"S"_s));
    CHECK(converted.messages.first().text.contains(u"<tool_call>"_s));
    // While One sits out, the chain answers for what Two can do.
    CHECK_FALSE(c.chain->capabilities().nativeTools);
}

TEST_CASE("createChain puts the first choice first and leaves out providers without fallback") {
    HttpClient http;
    ProviderRegistry registry(&http);
    registry.registerKind(u"stub"_s, [](const ProviderConfig &config, HttpClient *) {
        auto link = std::make_unique<FakeLink>(config.name);
        link->outcomes = {{{}, kDown}};
        return link;
    });
    Config config;
    for (const QString &name : {u"A"_s, u"B"_s, u"C"_s}) {
        config.providers.append(ProviderConfig{.id = name.toLower(), .kind = u"stub"_s, .name = name,
                                               .fallback = name != u"B"_s});
    }
    config.activeProvider = u"c"_s;
    Result<std::unique_ptr<IAiProvider>> created = registry.createChain(config);
    REQUIRE(created.isOk());
    std::unique_ptr<IAiProvider> chain = std::move(created).value();
    CHECK(chain->displayName() == u"C"_s);

    std::optional<Error> error;
    std::shared_ptr<ChatStream> stream = chain->chat({.messages = {{.role = Role::User, .text = u"hi"_s}}});
    QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &e) { error = e; });
    REQUIRE(QTest::qWaitFor([&] { return error.has_value(); }, kWaitMs));
    CHECK(error->message == u"C: down; A: down"_s);
}
