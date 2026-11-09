#include "core/ai/ProviderCatalog.h"

#include "core/ai/PresetFilter.h"
#include "core/config/Config.h"
#include "core/memory/MemoryStore.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <string>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

bool isLoopback(const QString &host) {
    return host == u"localhost"_s || host == u"127.0.0.1"_s || host == u"::1"_s;
}

QUrl parsed(const QString &text) {
    return QUrl(text, QUrl::StrictMode);
}

// A copy of an entry that must exist (a copy, so that a temporary id can be passed without the compiler
// suspecting a dangling reference).
CatalogEntry required(const QString &id) {
    const CatalogEntry *found = ProviderCatalog::find(id);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("The catalogue is not empty and every id is unique, lower-case and well formed") {
    const QList<CatalogEntry> &all = ProviderCatalog::entries();
    REQUIRE(all.size() >= 150);
    CHECK(qsizetype(catalogdata::kRowCount) == all.size());
    static const QRegularExpression wellFormed(u"^[a-z0-9-]+$"_s);
    QSet<QString> seen;
    for (const CatalogEntry &entry : all) {
        INFO(entry.id.toStdString());
        CHECK(wellFormed.match(entry.id).hasMatch());
        CHECK_FALSE(entry.id.startsWith(u'-'));
        CHECK_FALSE(seen.contains(entry.id));
        seen.insert(entry.id);
    }
}

TEST_CASE("Every entry has a name, a known kind and a category from the list") {
    const QStringList categories = ProviderCatalog::categories();
    const QStringList kinds{QString(kKindOpenAiCompatible), QString(kKindAnthropic), QString(kKindGemini)};
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        INFO(entry.id.toStdString());
        CHECK_FALSE(entry.name.isEmpty());
        CHECK(entry.name == entry.name.trimmed());
        CHECK(entry.name.size() <= 80);
        CHECK(kinds.contains(entry.kind));
        CHECK(categories.contains(entry.category));
    }
}

TEST_CASE("The categories come in display order, none is empty and Programs is not one of them") {
    const QStringList categories = ProviderCatalog::categories();
    CHECK(categories == QStringList{u"Gateways"_s, u"Model makers"_s, u"Inference hosts"_s, u"Regional"_s,
                                    u"Cloud platforms"_s, u"Local"_s, u"Free"_s});
    CHECK_FALSE(categories.contains(u"Programs"_s));
    QHash<QString, int> counts;
    for (const CatalogEntry &entry : ProviderCatalog::entries()) ++counts[entry.category];
    for (const QString &category : categories) {
        INFO(category.toStdString());
        CHECK(counts.value(category) > 0);
    }
}

TEST_CASE("The entries are sorted by category, then by name") {
    const QStringList categories = ProviderCatalog::categories();
    const QList<CatalogEntry> &all = ProviderCatalog::entries();
    for (qsizetype i = 1; i < all.size(); ++i) {
        const CatalogEntry &before = all.at(i - 1);
        const CatalogEntry &entry = all.at(i);
        INFO(before.id.toStdString() << " then " << entry.id.toStdString());
        const int categoryBefore = categories.indexOf(before.category);
        const int categoryNow = categories.indexOf(entry.category);
        REQUIRE(categoryBefore <= categoryNow);
        if (categoryBefore == categoryNow) {
            CHECK(QString::compare(before.name, entry.name, Qt::CaseInsensitive) <= 0);
        }
    }
}

TEST_CASE("Base URLs are http or https with a host, and are bases, not endpoints") {
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        INFO(entry.id.toStdString() << " " << entry.baseUrl.toStdString());
        const QUrl url = parsed(entry.baseUrl);
        REQUIRE(url.isValid());
        CHECK((url.scheme() == u"https"_s || url.scheme() == u"http"_s));
        CHECK_FALSE(url.host().isEmpty());
        CHECK(url.query().isEmpty());
        CHECK(url.fragment().isEmpty());
        CHECK(url.userInfo().isEmpty());
        CHECK_FALSE(entry.baseUrl.endsWith(u'/'));
        CHECK_FALSE(entry.baseUrl.endsWith(u"/chat/completions"_s));
        CHECK_FALSE(entry.baseUrl.endsWith(u"/messages"_s));
        CHECK_FALSE(entry.baseUrl.endsWith(u"/responses"_s));
        CHECK_FALSE(entry.baseUrl.contains(u"{"_s));
        CHECK_FALSE(entry.baseUrl.contains(u"<"_s));
        // Plain http only to this computer: a key never travels unencrypted over the network.
        if (url.scheme() == u"http"_s) CHECK(isLoopback(url.host()));
    }
}

TEST_CASE("A models URL is either empty or a complete http(s) address") {
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        if (entry.modelsUrl.isEmpty()) continue;
        INFO(entry.id.toStdString() << " " << entry.modelsUrl.toStdString());
        const QUrl url = parsed(entry.modelsUrl);
        REQUIRE(url.isValid());
        CHECK(url.scheme() == u"https"_s);
        CHECK_FALSE(url.host().isEmpty());
        CHECK(url.userInfo().isEmpty());
        // Only when it is not where the kind looks anyway.
        CHECK(entry.modelsUrl != entry.baseUrl + u"/models"_s);
    }
}

TEST_CASE("Websites are https addresses with a host, free of referral parameters") {
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        INFO(entry.id.toStdString() << " " << entry.website.toStdString());
        const QUrl url = parsed(entry.website);
        REQUIRE(url.isValid());
        CHECK(url.scheme() == u"https"_s);
        CHECK_FALSE(url.host().isEmpty());
        CHECK_FALSE(entry.website.contains(u"omniroute"_s, Qt::CaseInsensitive));
        CHECK_FALSE(entry.website.contains(u"aff="_s));
        CHECK_FALSE(entry.website.contains(u"utm_"_s));
    }
}

TEST_CASE("A key is required unless the entry is a local server or works without one") {
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        INFO(entry.id.toStdString());
        const bool keyless = entry.category == u"Local"_s || entry.category == u"Free"_s;
        CHECK(entry.requiresKey == !keyless);
        if (entry.category == u"Local"_s) {
            CHECK(isLoopback(parsed(entry.baseUrl).host()));
        } else {
            CHECK_FALSE(isLoopback(parsed(entry.baseUrl).host()));
        }
        // Without a key there is nothing to pay for: the free-tier mark says so.
        if (!entry.requiresKey && entry.category == u"Free"_s) CHECK(entry.freeTier);
    }
}

TEST_CASE("No two entries share a base URL, except local servers that share a default port") {
    QHash<QString, QString> owners;   // kind + lower-case base URL -> first id
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        const QString key = entry.kind + u'|' + entry.baseUrl.toLower();
        const auto earlier = owners.constFind(key);
        if (earlier == owners.constEnd()) {
            owners.insert(key, entry.id);
            continue;
        }
        INFO(entry.id.toStdString() << " shares " << entry.baseUrl.toStdString() << " with " << earlier.value().toStdString());
        CHECK(entry.category == u"Local"_s);
        CHECK(ProviderCatalog::find(earlier.value())->category == u"Local"_s);
    }
}

TEST_CASE("Hints are one short line and nothing in the data looks like a key") {
    static const QRegularExpression longHex(u"[0-9a-fA-F]{32,}"_s);
    static const QRegularExpression keyPrefix(u"\\b(?:sk-|AIza|ghp_|xox[abp]-|AKIA)"_s);
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        INFO(entry.id.toStdString());
        CHECK(entry.hint.size() <= 140);
        CHECK_FALSE(entry.hint.contains(u'\n'));
        CHECK(entry.hint == entry.hint.trimmed());
        CHECK_FALSE(entry.hint.contains(u"omniroute"_s, Qt::CaseInsensitive));
        const QStringList texts{entry.id,      entry.name,    entry.kind, entry.baseUrl,
                                entry.modelsUrl, entry.category, entry.website, entry.hint};
        for (const QString &text : texts) {
            INFO(text.toStdString());
            CHECK(MemoryStore::looksSecret(text).isEmpty());
            CHECK_FALSE(longHex.match(text).hasMatch());
            CHECK_FALSE(keyPrefix.match(text).hasMatch());
        }
    }
}

TEST_CASE("find() returns the entry with that id, and nothing for any other") {
    const QList<CatalogEntry> &all = ProviderCatalog::entries();
    for (const CatalogEntry &entry : all) {
        const CatalogEntry *found = ProviderCatalog::find(entry.id);
        REQUIRE(found != nullptr);
        CHECK(found == &entry);                       // the same object, not a copy
    }
    CHECK(ProviderCatalog::find(u"no-such-provider"_s) == nullptr);
    CHECK(ProviderCatalog::find({}) == nullptr);
    CHECK(ProviderCatalog::find(u"GROQ"_s) == nullptr);      // ids are lower-case
    CHECK(ProviderCatalog::find(u" groq"_s) == nullptr);
    CHECK(&ProviderCatalog::entries() == &all);               // built once
}

TEST_CASE("The providers Nexa already knows by hand are in the catalogue with the same hosts") {
    struct Expected { const char *id; const char *kind; const char *host; };
    const Expected expected[] = {
        {"groq", "openai-compatible", "api.groq.com"},
        {"openrouter", "openai-compatible", "openrouter.ai"},
        {"deepseek", "openai-compatible", "api.deepseek.com"},
        {"mistral", "openai-compatible", "api.mistral.ai"},
        {"xai", "openai-compatible", "api.x.ai"},
        {"together", "openai-compatible", "api.together.xyz"},
        {"cerebras", "openai-compatible", "api.cerebras.ai"},
        {"fireworks", "openai-compatible", "api.fireworks.ai"},
        {"nvidia", "openai-compatible", "integrate.api.nvidia.com"},
        {"huggingface", "openai-compatible", "router.huggingface.co"},
        {"perplexity", "openai-compatible", "api.perplexity.ai"},
        {"openai", "openai-compatible", "api.openai.com"},
        {"anthropic", "anthropic", "api.anthropic.com"},
        {"gemini", "gemini", "generativelanguage.googleapis.com"},
    };
    for (const Expected &want : expected) {
        const CatalogEntry &entry = required(QString::fromLatin1(want.id));
        INFO(std::string(want.id));
        CHECK(entry.kind == QString::fromLatin1(want.kind));
        CHECK(parsed(entry.baseUrl).host() == QString::fromLatin1(want.host));
        CHECK(entry.requiresKey);
        CHECK(entry.baseUrl.startsWith(u"https://"_s));
    }
}

TEST_CASE("Spot checks: the base is what Nexa's own provider appends its path to") {
    // OpenAI-compatible: Nexa posts to {base}/chat/completions.
    CHECK(required(u"openai"_s).baseUrl == u"https://api.openai.com/v1"_s);
    CHECK(required(u"groq"_s).baseUrl == u"https://api.groq.com/openai/v1"_s);
    CHECK(required(u"openrouter"_s).baseUrl == u"https://openrouter.ai/api/v1"_s);
    CHECK(required(u"fireworks"_s).baseUrl == u"https://api.fireworks.ai/inference/v1"_s);
    // Anthropic: {base}/messages, with x-api-key and anthropic-version.
    CHECK(required(u"anthropic"_s).baseUrl == u"https://api.anthropic.com/v1"_s);
    // Gemini: {base}/models/{model}:streamGenerateContent.
    CHECK(required(u"gemini"_s).baseUrl == u"https://generativelanguage.googleapis.com/v1beta"_s);
    // Every anthropic-kind entry ends at a versioned root, not at /messages.
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        if (entry.kind == QString(kKindAnthropic)) {
            INFO(entry.id.toStdString());
            CHECK_FALSE(entry.baseUrl.endsWith(u"/messages"_s));
        }
    }
}

TEST_CASE("A models list that is not at {base}/models has its address set") {
    // Perplexity's list is under /v1 while its chat endpoint is not.
    CHECK(required(u"perplexity"_s).modelsUrl == u"https://api.perplexity.ai/v1/models"_s);
    CHECK(required(u"aimlapi"_s).modelsUrl == u"https://api.aimlapi.com/models"_s);
    CHECK(required(u"deepseek"_s).modelsUrl == u"https://api.deepseek.com/v1/models"_s);
    // The ones that answer in a shape Nexa cannot read (Ollama's /api/tags) keep the default.
    CHECK(required(u"ollama-cloud"_s).modelsUrl.isEmpty());
    CHECK(required(u"fireworks"_s).modelsUrl.isEmpty());
}

TEST_CASE("Local servers carry their default port and need no key") {
    const CatalogEntry &ollama = required(u"ollama"_s);
    CHECK(ollama.baseUrl == u"http://localhost:11434/v1"_s);
    CHECK(ollama.category == u"Local"_s);
    CHECK_FALSE(ollama.requiresKey);
    CHECK(required(u"lmstudio"_s).baseUrl == u"http://localhost:1234/v1"_s);
    CHECK(required(u"vllm"_s).category == u"Local"_s);
    CHECK(required(u"llama-cpp"_s).category == u"Local"_s);
}

TEST_CASE("Nothing the catalogue lists needs a sign-in through a vendor's program") {
    // Claude, Codex, Antigravity, Cursor, Kiro, Copilot and Grok CLI are reached by their own programs, not by a key.
    for (const char *id : {"claude", "codex", "antigravity", "agy", "cursor", "kiro", "github", "grok-cli", "opencode"}) {
        CHECK(ProviderCatalog::find(QString::fromLatin1(id)) == nullptr);
    }
}

TEST_CASE("Every catalogue entry can be searched by the gallery") {
    // The gallery's search text comes from these fields: all of them must be usable as it is.
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        INFO(entry.id.toStdString());
        const QString host = PresetFilter::hostOf(entry.baseUrl);
        CHECK_FALSE(host.isEmpty());
        const QList<SearchableEntry> one{
            SearchableEntry{entry.id, entry.name, entry.category, host, entry.freeTier, entry.requiresKey}};
        CHECK(PresetFilter::filter(one, entry.id, {}) == QList<int>{0});
        CHECK(PresetFilter::filter(one, host, entry.category) == QList<int>{0});
    }
}

TEST_CASE("Docker Model Runner is at its documented OpenAI base, /engines/v1") {
    // The runtime answers its OpenAI API under /engines/v1 on port 12434, not under a bare /v1.
    const CatalogEntry &runner = required(u"docker-model-runner"_s);
    CHECK(runner.baseUrl == u"http://localhost:12434/engines/v1"_s);
    CHECK(runner.category == u"Local"_s);
    CHECK_FALSE(runner.requiresKey);
}

TEST_CASE("Z.AI points at the international platform its base URL belongs to") {
    // api.z.ai takes keys made at z.ai; open.bigmodel.cn is the separate China platform.
    const CatalogEntry &zai = required(u"zai"_s);
    CHECK(parsed(zai.baseUrl).host() == u"api.z.ai"_s);
    CHECK(parsed(zai.website).host() == u"z.ai"_s);
    CHECK_FALSE(zai.hint.contains(u"bigmodel"_s, Qt::CaseInsensitive));
}

TEST_CASE("Entries whose chat endpoint is not an OpenAI-compatible one are not listed") {
    // Coze's chat is /v3/chat with a bot id.
    CHECK(ProviderCatalog::find(u"coze"_s) == nullptr);
}

TEST_CASE("Hints do not tell the reader to use controls that exist only in another program") {
    // OmniRoute's notes talk about its own region switch and account connection; Nexa has neither.
    for (const CatalogEntry &entry : ProviderCatalog::entries()) {
        INFO(entry.id.toStdString() << ": " << entry.hint.toStdString());
        CHECK_FALSE(entry.hint.contains(u"select its"_s, Qt::CaseInsensitive));
        CHECK_FALSE(entry.hint.contains(u"connect your"_s, Qt::CaseInsensitive));
        CHECK_FALSE(entry.hint.contains(u"dashboard"_s, Qt::CaseInsensitive));
    }
}
