#include "core/ai/PresetFilter.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

SearchableEntry entry(const QString &id, const QString &name, const QString &category, const QString &host,
                      bool freeTier = false, bool requiresKey = true) {
    return SearchableEntry{id, name, category, host, freeTier, requiresKey};
}

// A small catalogue with the cases the gallery meets: brand names, a gateway, a local server, free services.
QList<SearchableEntry> sample() {
    return {
        entry(u"groq"_s, u"Groq"_s, u"Model makers"_s, u"api.groq.com"_s, true),                           // 0
        entry(u"openrouter"_s, u"OpenRouter"_s, u"Gateways"_s, u"openrouter.ai"_s, true),                  // 1
        entry(u"together"_s, u"Together AI"_s, u"Inference hosts"_s, u"api.together.xyz"_s),               // 2
        entry(u"ollama"_s, u"Ollama"_s, u"Local"_s, u"localhost:11434"_s, false, false),                   // 3
        entry(u"aion"_s, u"Aion Labs"_s, u"Gateways"_s, u"api.aionlabs.ai"_s, true),                       // 4
        entry(u"g4f-groq"_s, u"g4f.space - Groq"_s, u"Free"_s, u"g4f.space"_s, true, false),               // 5
        entry(u"huggingface"_s, u"HuggingFace"_s, u"Inference hosts"_s, u"router.huggingface.co"_s, true), // 6
        entry(u"ai-gateway"_s, u"AI Gateway"_s, u"Gateways"_s, u"gateway.example.com"_s),                  // 7
    };
}

} // namespace

TEST_CASE("PresetFilter keeps the order and everything for an empty query and category") {
    const QList<SearchableEntry> all = sample();
    const QList<int> everything{0, 1, 2, 3, 4, 5, 6, 7};
    CHECK(PresetFilter::filter(all, {}, {}) == everything);
    CHECK(PresetFilter::filter(all, u""_s, u""_s) == everything);
    CHECK(PresetFilter::filter(all, u"   \t "_s, u""_s) == everything);
    CHECK(PresetFilter::filter({}, u"groq"_s, {}).isEmpty());
}

TEST_CASE("PresetFilter matches case-insensitively in id, name, host and category") {
    const QList<SearchableEntry> all = sample();
    CHECK(PresetFilter::filter(all, u"HUGGINGFACE"_s, {}) == QList<int>{6});
    CHECK(PresetFilter::filter(all, u"hugging"_s, {}) == QList<int>{6});                 // id, name and host
    CHECK(PresetFilter::filter(all, u"together.xyz"_s, {}) == QList<int>{2});            // host only
    CHECK(PresetFilter::filter(all, u"localhost"_s, {}) == QList<int>{3});               // host only
    CHECK(PresetFilter::filter(all, u"11434"_s, {}) == QList<int>{3});                   // the port is in the host
    CHECK(PresetFilter::filter(all, u"inference hosts"_s, {}) == QList<int>{2, 6});      // category (two words)
    CHECK(PresetFilter::filter(all, u"nothing like this"_s, {}).isEmpty());
}

TEST_CASE("PresetFilter wants every word of the query") {
    const QList<SearchableEntry> all = sample();
    CHECK(PresetFilter::filter(all, u"open router"_s, {}) == QList<int>{1});             // both words inside "openrouter"
    CHECK(PresetFilter::filter(all, u"router open"_s, {}) == QList<int>{1});             // order does not matter
    CHECK(PresetFilter::filter(all, u"groq api"_s, {}) == QList<int>{0});                // "api" is only in api.groq.com
    CHECK(PresetFilter::filter(all, u"groq space"_s, {}) == QList<int>{5});              // "space" is only in g4f.space
    CHECK(PresetFilter::filter(all, u"groq zzz"_s, {}).isEmpty());
    CHECK(PresetFilter::filter(all, u"  groq   labs "_s, {}).isEmpty());                 // Groq, and Labs is another entry
}

TEST_CASE("PresetFilter ranks a name that starts with the word, then one that contains it, then the rest") {
    const QList<SearchableEntry> all = sample();
    // "ai": names starting with it (Aion Labs, AI Gateway), then a name containing it (Together AI), then the rest,
    // here a host-only hit (openrouter.ai).
    CHECK(PresetFilter::filter(all, u"ai"_s, {}) == QList<int>{4, 7, 2, 1});
    // Equal rank keeps the order of the list: the two names starting with "g" (Groq, g4f.space - Groq).
    CHECK(PresetFilter::filter(all, u"g"_s, {}).mid(0, 2) == QList<int>{0, 5});
    // "groq": name-prefix Groq (0), then the name that merely contains it (5).
    CHECK(PresetFilter::filter(all, u"groq"_s, {}) == QList<int>{0, 5});
    // The ranking goes by the first word of the query that is not a special one.
    CHECK(PresetFilter::filter(all, u"free ai"_s, {}) == QList<int>{4, 1});
}

TEST_CASE("PresetFilter filters by category, empty meaning all") {
    const QList<SearchableEntry> all = sample();
    CHECK(PresetFilter::filter(all, {}, u"Gateways"_s) == QList<int>{1, 4, 7});
    CHECK(PresetFilter::filter(all, {}, u"gateways"_s) == QList<int>{1, 4, 7});          // case does not matter
    CHECK(PresetFilter::filter(all, {}, u"Local"_s) == QList<int>{3});
    CHECK(PresetFilter::filter(all, {}, u"Programs"_s).isEmpty());                       // a category with nothing in it
    CHECK(PresetFilter::filter(all, u"groq"_s, u"Free"_s) == QList<int>{5});             // both at once
    CHECK(PresetFilter::filter(all, u"groq"_s, u"Gateways"_s).isEmpty());
}

TEST_CASE("PresetFilter: the word free also matches a free tier, nokey a provider without a key") {
    const QList<SearchableEntry> all = sample();
    // Free tier: groq, openrouter, aion, g4f-groq (also by its category, "Free"), huggingface.
    CHECK(PresetFilter::filter(all, u"free"_s, {}) == QList<int>{0, 1, 4, 5, 6});
    CHECK(PresetFilter::filter(all, u"FREE"_s, {}) == QList<int>{0, 1, 4, 5, 6});
    // nokey: Ollama and the g4f gateway.
    CHECK(PresetFilter::filter(all, u"nokey"_s, {}) == QList<int>{3, 5});
    // Combined with other words and with a category.
    CHECK(PresetFilter::filter(all, u"free groq"_s, {}) == QList<int>{0, 5});
    CHECK(PresetFilter::filter(all, u"nokey groq"_s, {}) == QList<int>{5});
    CHECK(PresetFilter::filter(all, u"free"_s, u"Gateways"_s) == QList<int>{1, 4});
    CHECK(PresetFilter::filter(all, u"free nokey"_s, {}) == QList<int>{5});
    // A word that only looks like them is plain text.
    CHECK(PresetFilter::filter(all, u"freebie"_s, {}).isEmpty());
}

TEST_CASE("PresetFilter hands back indices into the list it was given") {
    QList<SearchableEntry> shuffled = sample();
    std::reverse(shuffled.begin(), shuffled.end());
    const QList<int> found = PresetFilter::filter(shuffled, u"groq"_s, {});
    REQUIRE(found.size() == 2);
    // Reversed list: g4f-groq is now at 2, Groq at 7; Groq's name starts with the word, so it comes first.
    CHECK(shuffled.at(found.at(0)).id == u"groq"_s);
    CHECK(shuffled.at(found.at(1)).id == u"g4f-groq"_s);
}

TEST_CASE("PresetFilter::hostOf names the host the gallery shows") {
    CHECK(PresetFilter::hostOf(u"https://api.groq.com/openai/v1"_s) == u"api.groq.com"_s);
    CHECK(PresetFilter::hostOf(u"HTTPS://API.Example.COM/V1"_s) == u"api.example.com"_s);
    CHECK(PresetFilter::hostOf(u"  https://openrouter.ai/api/v1  "_s) == u"openrouter.ai"_s);
    CHECK(PresetFilter::hostOf(u"http://localhost:11434/v1"_s) == u"localhost:11434"_s);
    CHECK(PresetFilter::hostOf(u"http://127.0.0.1:8080/v1"_s) == u"127.0.0.1:8080"_s);
    CHECK(PresetFilter::hostOf(u"http://[::1]:8080/v1"_s) == u"[::1]:8080"_s);
    CHECK(PresetFilter::hostOf(u"https://api.deepseek.com"_s) == u"api.deepseek.com"_s);
    CHECK(PresetFilter::hostOf(u"api.together.xyz/v1"_s) == u"api.together.xyz"_s);      // no scheme
    // A password in the URL is never part of what is shown.
    CHECK(PresetFilter::hostOf(u"https://user:secret@host.example/v1"_s) == u"host.example"_s);
    CHECK(PresetFilter::hostOf({}).isEmpty());
    CHECK(PresetFilter::hostOf(u"   "_s).isEmpty());
    CHECK(PresetFilter::hostOf(u"not a url"_s).isEmpty());
}

TEST_CASE("PresetFilter reads regular-expression characters in a query as plain text") {
    const QList<SearchableEntry> all = sample();
    for (const QString &query : {u"("_s, u"["_s, u"*"_s, u".*"_s, u"[a-z]+"_s, u"\\"_s, u"^groq$"_s, u"gro|ope"_s, u"?"_s,
                                 u"a.i"_s, u"+"_s}) {
        INFO(query.toStdString());
        CHECK(PresetFilter::filter(all, query, {}).isEmpty());
    }
    // A dot is a dot: "groq.com" is in api.groq.com, "groq.com" is not "groqXcom".
    CHECK(PresetFilter::filter(all, u"groq.com"_s, {}) == QList<int>{0});
    CHECK(PresetFilter::filter(all, u"api.together.xyz"_s, {}) == QList<int>{2});
    // The same characters in a category are literal too.
    CHECK(PresetFilter::filter(all, {}, u".*"_s).isEmpty());
}

TEST_CASE("PresetFilter folds case and blanks beyond ASCII") {
    const QList<SearchableEntry> all{
        entry(u"ecole"_s, u"École IA"_s, u"Regional"_s, u"ecole.example"_s),                // Ecole IA
        entry(u"zurich"_s, u"Zürich AI"_s, u"Regional"_s, u"zurich.example"_s),
        entry(u"athens"_s, u"Αθήνα"_s, u"Regional"_s, u"athens.example"_s),   // Athina, Greek
    };
    CHECK(PresetFilter::filter(all, u"école"_s, {}) == QList<int>{0});
    CHECK(PresetFilter::filter(all, u"ÉCOLE"_s, {}) == QList<int>{0});
    CHECK(PresetFilter::filter(all, u"ZÜRICH"_s, {}) == QList<int>{1});
    CHECK(PresetFilter::filter(all, u"ΑΘήΝΑ"_s, {}) == QList<int>{2});
    // A no-break space or an ideographic space between words separates them like a blank.
    CHECK(PresetFilter::filter(all, u"zürich ai"_s, {}) == QList<int>{1});
    CHECK(PresetFilter::filter(all, u"école　ia"_s, {}) == QList<int>{0});
    // Letters that only look alike do not match.
    CHECK(PresetFilter::filter(all, u"ecole"_s, {}) == QList<int>{0});                          // the id and the host
    CHECK(PresetFilter::filter(all, u"zurich ai"_s, {}) == QList<int>{1});                      // via the id, name word "AI"
}

TEST_CASE("PresetFilter ranks by the first text word even when a flag comes first, and ties keep list order") {
    const QList<SearchableEntry> all{
        entry(u"b"_s, u"Beta Groq"_s, u"Gateways"_s, u"b.example"_s, true),     // 0: contains
        entry(u"a"_s, u"Groq Alpha"_s, u"Gateways"_s, u"a.example"_s, true),    // 1: starts with
        entry(u"c"_s, u"Gamma"_s, u"Gateways"_s, u"groq.example"_s, true),      // 2: host only
        entry(u"d"_s, u"Groq Delta"_s, u"Gateways"_s, u"d.example"_s, false),   // 3: starts with, no free tier
    };
    CHECK(PresetFilter::filter(all, u"free groq"_s, {}) == QList<int>{1, 0, 2});
    CHECK(PresetFilter::filter(all, u"groq free"_s, {}) == QList<int>{1, 0, 2});
    CHECK(PresetFilter::filter(all, u"groq"_s, {}) == QList<int>{1, 3, 0, 2});
}

TEST_CASE("PresetFilter::hostOf copes with odd input") {
    CHECK(PresetFilter::hostOf(u"http://"_s).isEmpty());
    CHECK(PresetFilter::hostOf(u"https:///v1"_s).isEmpty());
    CHECK(PresetFilter::hostOf(u"://"_s).isEmpty());
    CHECK(PresetFilter::hostOf(u"localhost:11434"_s) == u"localhost:11434"_s);
    CHECK(PresetFilter::hostOf(u"https://api.example.com:8443/v1"_s) == u"api.example.com:8443"_s);
}
