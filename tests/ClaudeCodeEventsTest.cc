// core/ai/ClaudeCodeEvents: Claude Code's stream-json and `auth status --json`, as measured against Claude Code 2.1.291
// on 2026-11-12 (the lines below are from a real two-turn session on `haiku` and a run in a config folder nobody was
// signed in to, shortened; no account of anyone is in them).

#include "core/ai/ClaudeCodeEvents.h"

#include "core/ai/ProviderChain.h"
#include "core/ai/ProviderRegistry.h"

#include "QtStringMakers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

const char *kInit =
    R"({"type":"system","subtype":"init","cwd":"/tmp/nexa-cc/work","session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","tools":[],"mcp_servers":[],"model":"claude-haiku-4-5-20251001","permissionMode":"dontAsk","slash_commands":[],"apiKeySource":"none","claude_code_version":"2.1.291","output_style":"default","agents":["claude","Explore"],"skills":[],"plugins":[],"uuid":"92269669-f03b-40a1-a637-9d791e09f181"})";
const char *kStatus =
    R"({"type":"system","subtype":"status","status":"requesting","session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","uuid":"8fa04aa5-ce52-47d9-b5b8-403ee60bb705"})";
const char *kMessageStart =
    R"({"type":"stream_event","event":{"type":"message_start","message":{"model":"claude-haiku-4-5-20251001","id":"msg_011CgwffLw68ekVSJk5TLWvd","type":"message","role":"assistant","content":[],"stop_reason":null,"usage":{"input_tokens":506,"output_tokens":6}}},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"x"})";
const char *kThinkingStart =
    R"({"type":"stream_event","event":{"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"117c8e91","thinking_display":"updates"})";
const char *kThinkingDelta =
    R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"The user wants","estimated_tokens":null}},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"3cd53b48","thinking_display":"updates"})";
const char *kSignatureDelta =
    R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"EpIDCtABCBMYAipAmliF9cKqfJYe6nIfsyKM"}},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"y"})";
const char *kTextStart =
    R"({"type":"stream_event","event":{"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"3037f6b6"})";
const char *kTextDelta =
    R"({"type":"stream_event","event":{"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"OK one"}},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"0be12d09"})";
const char *kAssistant =
    R"({"type":"assistant","message":{"model":"claude-haiku-4-5-20251001","id":"msg_011CgwffLw68ekVSJk5TLWvd","type":"message","role":"assistant","content":[{"type":"text","text":"OK one"}],"stop_reason":null,"usage":{"input_tokens":506,"output_tokens":6}},"parent_tool_use_id":null,"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","uuid":"z"})";
const char *kBlockStop =
    R"({"type":"stream_event","event":{"type":"content_block_stop","index":1},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"b227c5fd"})";
const char *kMessageDelta =
    R"({"type":"stream_event","event":{"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"input_tokens":506,"output_tokens":35,"output_tokens_details":{"thinking_tokens":27}}},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"a38f2727"})";
const char *kMessageStop =
    R"({"type":"stream_event","event":{"type":"message_stop"},"session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","parent_tool_use_id":null,"uuid":"18f9b4e9"})";
const char *kRateLimit =
    R"({"type":"rate_limit_event","rate_limit_info":{"status":"allowed","resetsAt":1794432600,"rateLimitType":"five_hour","overageStatus":"rejected","isUsingOverage":false,"unifiedWindows":{"five_hour":{"utilization":0.59,"resetsAt":1794432600}}},"uuid":"e2b57de9","session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe"})";
const char *kResult =
    R"({"duration_api_ms":1117,"stop_reason":"end_turn","session_id":"b1f1b967-9687-49a7-af51-b27b048e16fe","total_cost_usd":0.000681,"usage":{"input_tokens":506,"output_tokens":35},"modelUsage":{},"permission_denials":[],"terminal_reason":"completed","is_error":false,"num_turns":1,"subtype":"success","api_error_status":null,"result":"OK one","type":"result","duration_ms":1216,"uuid":"45bf81a1"})";
// Nobody signed in (a run on an empty config folder): the API refused the turn, Claude Code said so as an `assistant`
// message with an error name and as a result that is still "success" by subtype but is_error, then exited 1.
const char *kRefusedAssistant =
    R"({"type":"assistant","message":{"id":"80a404d4","model":"<synthetic>","role":"assistant","stop_reason":"stop_sequence","stop_sequence":"","type":"message","usage":{"input_tokens":0,"output_tokens":0},"content":[{"type":"text","text":"Not logged in · Please run /login"}]},"parent_tool_use_id":null,"session_id":"bd236391","uuid":"bfea68ad","timestamp":"2026-11-11T19:37:38.254Z","error":"authentication_failed"})";
const char *kRefusedResult =
    R"({"duration_api_ms":0,"stop_reason":"stop_sequence","session_id":"bd236391","total_cost_usd":0,"usage":{"input_tokens":0,"output_tokens":0},"modelUsage":{},"permission_denials":[],"terminal_reason":"api_error","is_error":true,"num_turns":1,"subtype":"success","api_error_status":null,"result":"Not logged in · Please run /login","type":"result","duration_ms":14,"uuid":"c1"})";

} // namespace

TEST_CASE("decode reads Claude Code's init, text and result events") {
    const ClaudeCodeEvent init = ClaudeCodeEvents::decode(kInit);
    CHECK(init.kind == ClaudeCodeEvent::Kind::Init);
    CHECK(init.model == u"claude-haiku-4-5-20251001"_s);
    CHECK(init.sessionId == u"b1f1b967-9687-49a7-af51-b27b048e16fe"_s);

    const ClaudeCodeEvent text = ClaudeCodeEvents::decode(kTextDelta);
    CHECK(text.kind == ClaudeCodeEvent::Kind::Text);
    CHECK(text.text == u"OK one"_s);

    const ClaudeCodeEvent result = ClaudeCodeEvents::decode(kResult);
    CHECK(result.kind == ClaudeCodeEvent::Kind::Result);
    CHECK(result.ok);
    CHECK(result.subtype == u"success"_s);
    CHECK(result.response == u"OK one"_s);
    CHECK(result.error.isEmpty());
    CHECK(result.apiErrorStatus == 0);
}

TEST_CASE("Only a text delta is reply text: thinking, signatures, the whole message and the bookkeeping are not") {
    for (const char *line : {kStatus, kMessageStart, kThinkingStart, kThinkingDelta, kSignatureDelta, kTextStart,
                             kAssistant, kBlockStop, kMessageDelta, kMessageStop, kRateLimit}) {
        CAPTURE(line);
        CHECK(ClaudeCodeEvents::decode(line).kind == ClaudeCodeEvent::Kind::Ignored);
    }
}

TEST_CASE("decode reads a turn the API refused") {
    const ClaudeCodeEvent refused = ClaudeCodeEvents::decode(kRefusedAssistant);
    CHECK(refused.kind == ClaudeCodeEvent::Kind::ApiError);
    CHECK(refused.apiError == u"authentication_failed"_s);

    const ClaudeCodeEvent result = ClaudeCodeEvents::decode(kRefusedResult);
    CHECK(result.kind == ClaudeCodeEvent::Kind::Result);
    CHECK_FALSE(result.ok);                 // is_error wins over the subtype
    CHECK(result.error == u"Not logged in · Please run /login"_s);
    CHECK(result.response.isEmpty());

    // A result that only names its failure, and one with an API status.
    const ClaudeCodeEvent named = ClaudeCodeEvents::decode(
        R"({"type":"result","subtype":"error_during_execution","is_error":true,"result":"","errors":["something broke"],"api_error_status":529})");
    CHECK_FALSE(named.ok);
    CHECK(named.error == u"something broke"_s);
    CHECK(named.apiErrorStatus == 529);
    const ClaudeCodeEvent bare = ClaudeCodeEvents::decode(R"({"type":"result","subtype":"error_max_turns","is_error":true})");
    CHECK_FALSE(bare.ok);
    CHECK(bare.error == u"error_max_turns"_s);
}

TEST_CASE("Claude Code's decoder ignores what it does not know") {
    CHECK(ClaudeCodeEvents::decode("").kind == ClaudeCodeEvent::Kind::Ignored);
    CHECK(ClaudeCodeEvents::decode("this is not json").kind == ClaudeCodeEvent::Kind::Ignored);
    CHECK(ClaudeCodeEvents::decode(R"({"type":"user","message":{}})").kind == ClaudeCodeEvent::Kind::Ignored);
    CHECK(ClaudeCodeEvents::decode(R"({"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"input_json_delta","partial_json":"{"}}})").kind
          == ClaudeCodeEvent::Kind::Ignored);
}

TEST_CASE("userTurn writes one Messages line, the pictures before the words") {
    const QString said = u"Say \"hi\"\nthen stop"_s;
    const QByteArray plain = ClaudeCodeEvents::userTurn(said);
    REQUIRE(plain.endsWith('\n'));
    CHECK(plain.count('\n') == 1);
    const QJsonObject object = QJsonDocument::fromJson(plain).object();
    CHECK(object.value(u"type"_s).toString() == u"user"_s);
    const QJsonObject message = object.value(u"message"_s).toObject();
    CHECK(message.value(u"role"_s).toString() == u"user"_s);
    QJsonArray content = message.value(u"content"_s).toArray();
    REQUIRE(content.size() == 1);
    CHECK(content.at(0).toObject().value(u"type"_s).toString() == u"text"_s);
    CHECK(content.at(0).toObject().value(u"text"_s).toString() == said);

    const QList<ImageAttachment> images{{QByteArray("fake-png"), u"image/png"_s}};
    content = QJsonDocument::fromJson(ClaudeCodeEvents::userTurn(u"what is this?"_s, images))
                  .object().value(u"message"_s).toObject().value(u"content"_s).toArray();
    REQUIRE(content.size() == 2);
    const QJsonObject image = content.at(0).toObject();
    CHECK(image.value(u"type"_s).toString() == u"image"_s);
    const QJsonObject source = image.value(u"source"_s).toObject();
    CHECK(source.value(u"type"_s).toString() == u"base64"_s);
    CHECK(source.value(u"media_type"_s).toString() == u"image/png"_s);
    CHECK(QByteArray::fromBase64(source.value(u"data"_s).toString().toLatin1()) == "fake-png");
    CHECK(content.at(1).toObject().value(u"text"_s).toString() == u"what is this?"_s);
}

TEST_CASE("knownModels are Claude Code's aliases for its latest models") {
    CHECK(ClaudeCodeEvents::knownModels() == QStringList{u"fable"_s, u"opus"_s, u"sonnet"_s, u"haiku"_s});
}

TEST_CASE("parseStatus keeps the plan and drops the account") {
    // The shape of `claude auth status --json` signed in (2.1.291), with the keys Nexa must never read filled in.
    const Result<SignInStatus> signedIn = ClaudeCodeEvents::parseStatus(
        R"({"loggedIn":true,"authMethod":"claude.ai","apiProvider":"firstParty","analyticsDisabled":true,"subscriptionType":"max","email":"private.person@example.invalid","orgName":"Private Org","orgId":"org_SECRET"})");
    REQUIRE(signedIn.isOk());
    CHECK(signedIn.value().state == SignInStatus::State::SignedIn);
    CHECK(signedIn.value().detail == u"Claude · Max"_s);
    CHECK_FALSE(signedIn.value().detail.contains(u"private"_s));
    CHECK_FALSE(signedIn.value().detail.contains(u"Org"_s));
    CHECK(signedIn.value().trouble.isEmpty());

    const Result<SignInStatus> pro = ClaudeCodeEvents::parseStatus(R"({"loggedIn":true,"authMethod":"claude.ai","subscriptionType":"pro"})");
    REQUIRE(pro.isOk());
    CHECK(pro.value().detail == u"Claude · Pro"_s);
    const Result<SignInStatus> noPlan = ClaudeCodeEvents::parseStatus(R"({"loggedIn":true,"authMethod":"claude.ai"})");
    REQUIRE(noPlan.isOk());
    CHECK(noPlan.value().detail == u"Claude"_s);
    const Result<SignInStatus> console = ClaudeCodeEvents::parseStatus(R"({"loggedIn":true,"authMethod":"console"})");
    REQUIRE(console.isOk());
    CHECK(console.value().detail.contains(u"Console"_s));
    // An unknown plan is kept to plain characters.
    const Result<SignInStatus> odd = ClaudeCodeEvents::parseStatus(R"({"loggedIn":true,"authMethod":"claude.ai","subscriptionType":"team_plus <x>"})");
    REQUIRE(odd.isOk());
    CHECK(odd.value().detail == u"Claude · Team plus x"_s);

    // Signed out (the real shape, with the config folder it names left out of the status).
    const Result<SignInStatus> signedOut = ClaudeCodeEvents::parseStatus(
        R"({"loggedIn":false,"authMethod":"none","apiProvider":"firstParty","analyticsDisabled":true,"projectsDirectory":"/tmp/x/projects","configDirectory":"/tmp/x"})");
    REQUIRE(signedOut.isOk());
    CHECK(signedOut.value().state == SignInStatus::State::SignedOut);
    CHECK(signedOut.value().detail.isEmpty());
}

TEST_CASE("A status that is not Claude Code's JSON is Parse, never 'signed out'") {
    for (const char *reply : {"", "not json", "[]", R"({"something":"else"})", R"({"loggedIn":"yes"})"}) {
        CAPTURE(reply);
        const Result<SignInStatus> status = ClaudeCodeEvents::parseStatus(reply);
        REQUIRE_FALSE(status.isOk());
        CHECK(status.error().code == ErrorCode::Parse);
    }
}

TEST_CASE("exitError says what to do") {
    const Error signIn = ClaudeCodeEvents::exitError(1, u"Not logged in · Please run /login\n"_s);
    CHECK(signIn.code == ErrorCode::Unauthorized);
    CHECK(signIn.message.contains(u"not signed in"_s));
    CHECK(ProviderRegistry::meansNotSignedIn(signIn));

    const Error crashed = ClaudeCodeEvents::exitError(2, u"first line\nsegmentation fault\n"_s);
    CHECK(crashed.code == ErrorCode::ConnectionRefused);
    CHECK(crashed.message == u"Claude Code stopped (exit 2): segmentation fault"_s);
    CHECK(ClaudeCodeEvents::exitError(3, {}).message == u"Claude Code stopped (exit 3): no message"_s);
}

TEST_CASE("resultError: the sign-in gone is 'not signed in', a limit is HTTP 429, a missing model NotFound") {
    ClaudeCodeEvent refused = ClaudeCodeEvents::decode(kRefusedResult);
    refused.apiError = u"authentication_failed"_s;        // as the session fills it in from the assistant event
    const Error unauthorized = ClaudeCodeEvents::resultError(refused);
    CHECK(unauthorized.code == ErrorCode::Unauthorized);
    CHECK(ProviderRegistry::meansNotSignedIn(unauthorized));
    CHECK(ProviderChain::meansUnavailable(unauthorized));
    // The words alone say it too.
    ClaudeCodeEvent words;
    words.ok = false;
    words.error = u"Invalid API key · Please run /login"_s;
    CHECK(ClaudeCodeEvents::resultError(words).code == ErrorCode::Unauthorized);
    words.error = u"OAuth token has expired"_s;
    CHECK(ClaudeCodeEvents::resultError(words).code == ErrorCode::Unauthorized);

    ClaudeCodeEvent limit;
    limit.ok = false;
    limit.error = u"You've hit your limit · resets 3pm (Asia/Karachi)"_s;
    Error quota = ClaudeCodeEvents::resultError(limit);
    CHECK(quota.code == ErrorCode::Http);
    CHECK(quota.message.startsWith(u"HTTP 429: Claude Code: "_s));
    CHECK(ProviderChain::meansUnavailable(quota));
    limit.error = u"API Error: 529 {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"_s;
    limit.apiErrorStatus = 529;
    quota = ClaudeCodeEvents::resultError(limit);
    CHECK(quota.message.startsWith(u"HTTP 529: "_s));
    CHECK(ProviderChain::meansUnavailable(quota));
    ClaudeCodeEvent byName;
    byName.ok = false;
    byName.error = u"something"_s;
    byName.apiError = u"rate_limit"_s;
    CHECK(ClaudeCodeEvents::resultError(byName).code == ErrorCode::Http);

    ClaudeCodeEvent missing;
    missing.ok = false;
    missing.error = u"API Error: 404 {\"type\":\"error\",\"error\":{\"type\":\"not_found_error\",\"message\":\"model: claude-9\"}}"_s;
    missing.apiErrorStatus = 404;
    const Error notFound = ClaudeCodeEvents::resultError(missing);
    CHECK(notFound.code == ErrorCode::NotFound);
    CHECK(notFound.message.contains(u"haiku"_s));
    CHECK(ProviderChain::meansUnavailable(notFound));

    ClaudeCodeEvent other;
    other.ok = false;
    other.error = u"something broke"_s;
    const Error unknown = ClaudeCodeEvents::resultError(other);
    CHECK(unknown.code == ErrorCode::Unknown);
    CHECK(unknown.message == u"Claude Code: something broke"_s);
    ClaudeCodeEvent silent;
    silent.ok = false;
    silent.subtype = u"error_max_turns"_s;
    CHECK(ClaudeCodeEvents::resultError(silent).message == u"Claude Code: error_max_turns"_s);
}
