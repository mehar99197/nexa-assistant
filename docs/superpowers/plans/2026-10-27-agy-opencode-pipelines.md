# agy and opencode pipelines (M15 part A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Nexa answers through agy (a warm CLI process) and opencode (its own private server), falls back along an ordered chain of providers, offers OpenCode's hosted API as presets, and no longer knows OmniRoute.

**Architecture:** Two new `IAiProvider` strategies in `core/ai/` drive the programs and report `nativeTools = false`, so the `Agent`'s existing `<tool_call>` text convention keeps every action in `ToolExecutor`. A pure `TurnLedger` decides per request whether the backend session continues (only new turns sent) or starts fresh (history written as a transcript). `ProviderChain` wraps the configured providers in order and moves on only when a link cannot answer and nothing has streamed.

**Tech Stack:** C++20, Qt 6.10 (Core, Network), Catch2 v3, Python 3 stand-ins for agy and opencode.

**Spec:** `docs/superpowers/specs/2026-10-27-agy-opencode-pipelines-design.md`

## Global Constraints

- Conventions of `CLAUDE.md`: PascalCase `.h`/`.cc` per class, `#pragma once`, `namespace nexa {` unindented, `u"…"_s` in `.cc`, `QStringLiteral` in headers, `Result<T>` / `failed(Error)` instead of exceptions, `camelCase_` members, design pattern named in the class comment, every source listed in its `CMakeLists.txt`.
- `core` never includes QtWidgets; nothing blocks the GUI thread (QProcess and QNetworkAccessManager signals only — no `waitFor*` outside destructors).
- Every action goes through `ToolExecutor::execute`: agy's and opencode's own tools never act; both providers report `Capabilities{.streaming = true, .nativeTools = false, …}`.
- Secrets: the opencode password is 32 random bytes as hex, set only in the child's environment (`OPENCODE_SERVER_PASSWORD`), never logged, never in argv or config (attach mode keeps the user's password in the provider's `apiKey`). Log category `lcAi`.
- Child processes: stdin `/dev/null` for opencode, a pipe for agy; `QProcess::UnixProcessFlag::CreateNewSession | ResetSignalHandlers | CloseFileDescriptors`; private working folders `~/.cache/nexa/agy` and `~/.cache/nexa/opencode` (created 0700); the whole process group killed on stop.
- Exact values: agy warm args `--input-format stream-json --output-format stream-json --disable-slash-commands --print=` (+ `--model <id>` when chosen); opencode args `serve --hostname 127.0.0.1 --port 0`; opencode start limit 30 s; agy idle stop 10 min; chain skip window 60 s; opencode retry give-up 30 s.
- Config `kVersion` becomes 3. OmniRoute appears nowhere in code, presets or `CLAUDE.md`/`README.md` once done (ARCHITECTURE.md history rows excepted).
- Build and test: `cmake --build build` then `./build/tests/nexa_tests "<test case name>"` for one case, `ctest --test-dir build --output-on-failure -j4` for all. The project path has a space: quote it.

## Review Focus

- The user changes the model in the toolbar mid-conversation → the next request starts a fresh session with the history as a transcript (TurnLedgerTest "A different model…").
- A long chat whose oldest turns `Conversation` trims away → the session continues instead of restarting every step (TurnLedgerTest "A trimmed history still continues").
- Nexa quits or crashes while opencode's server or agy runs → no process is left behind (OpenCodeProviderTest "Destroying the server stops the process", AgyProviderTest "Cancel kills the process…").
- opencode's free tier runs out and opencode retries silently → Nexa gives up at once and the chain moves on (OpenCodeProviderTest "A quota retry gives up at once…").
- A reply that is JSON or a code block but not a call → held while streaming, then shown in full (TextToolParserTest "A reply that is JSON but not a call…").

---

### Task 0: Branch

- [ ] **Step 1:** `git switch -c m15-agy-opencode` (from the current `m14-browser-memory-skills` HEAD, which holds M14). Expected: `Switched to a new branch 'm15-agy-opencode'`.

---

### Task 1: OmniRoute out, the new presets, config v3, program lookup, model-list URLs

**Files:**
- Create: `core/ai/ProgramLookup.h`, `core/ai/ProgramLookup.cc`
- Modify: `core/config/Config.h`, `core/config/Config.cc`, `core/ai/ProviderRegistry.h`, `core/ai/ProviderRegistry.cc`, `core/ai/OpenAiProvider.h/.cc`, `core/ai/AnthropicProvider.cc`, `core/CMakeLists.txt`, `app/main.cc:95-101`, `app/SettingsDialog.cc:194`
- Reword comments naming OmniRoute (behaviour unchanged): `core/agent/Agent.cc:114`, `app/AppController.cc:33`, `core/ai/AnthropicProvider.cc:81`, `app/MainWindow.cc:67`, `core/ai/OpenAiProvider.h:18`, `voice/stt/CloudRecognizer.h:16,24`, `voice/tts/CloudSynthesizer.h:16,17,22`, `voice/VoiceBackends.h:20`, `core/config/Config.h:17`
- Test: create `tests/ProviderRegistryTest.cc`; modify `tests/ConfigStoreTest.cc` (ids `omniroute` → `gateway`), `tests/VoicePureTest.cc:358-379`, `tests/VoicePipelineTest.cc:303,306` ("the provider is down"), `tests/OpenAiStreamTest.cc:72` ("a gateway's keep-alive"), `tests/OpenAiProviderTest.cc`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `inline constexpr QLatin1StringView kKindAgy{"agy"}; inline constexpr QLatin1StringView kKindOpenCode{"opencode"};` in `Config.h`
  - `ProviderConfig::fallback` (`bool`, default `true`, JSON `"fallback"`), `ProviderConfig::modelsUrl` (`QString`, JSON `"modelsUrl"`); `Config::kVersion = 3`
  - `QString findProgram(const QString &name, const QString &configured = {})` (`ProgramLookup.h`): `configured` non-empty → it if executable, else empty; otherwise `QStandardPaths::findExecutable(name)`, then the same in `~/.local/bin`
  - `ProviderPreset::modelsUrl` (last field)
  - `static QList<ProviderConfig> ProviderRegistry::defaultProviders(const std::function<bool(const QString &program)> &installed)` — `opencode` then `agy`, each only if `installed(name)`
  - `static void ProviderRegistry::upgrade(Config &config, const std::function<bool(const QString &program)> &installed)` — replaces the one-argument form
  - `static bool ProviderRegistry::isHttpKind(const QString &kind)` — true for openai-compatible, anthropic, gemini
  - `ProviderRegistry::defaultProvider()` is **removed**

- [ ] **Step 1: Write the failing tests** in `tests/ProviderRegistryTest.cc`:

```cpp
TEST_CASE("No preset is OmniRoute") {
    for (const ProviderPreset &preset : ProviderRegistry::presets()) {
        CHECK(preset.id != u"omniroute"_s);
        CHECK_FALSE(preset.baseUrl.contains(u":20128"_s));
    }
}
TEST_CASE("The opencode and agy presets need no key and no URL") {
    // find by id: kind kKindOpenCode / kKindAgy, requiresKey false, baseUrl empty, defaultModel empty
}
TEST_CASE("OpenCode's hosted API presets list models from their own URL") {
    // opencode-console: openai-compatible, "https://opencode.ai/inference/openai/v1", modelsUrl "https://opencode.ai/inference/v1/models", requiresKey
    // opencode-console-claude: anthropic, "https://opencode.ai/inference/anthropic/v1", same modelsUrl
    // opencode-go: "https://opencode.ai/inference/go/openai/v1", modelsUrl "https://opencode.ai/inference/go/v1/models"
    // opencode-go-claude: anthropic, "https://opencode.ai/inference/go/anthropic/v1", go modelsUrl
}
TEST_CASE("defaultProviders offers the installed pipelines, opencode first") {
    // both installed → ids {"opencode","agy"}; only "agy" → {"agy"}; none → empty
}
TEST_CASE("Upgrading a version 2 config adds the pipelines to an empty list, once") {
    // v2, no providers, both installed → ids {"opencode","agy"}, activeProvider "opencode", version 3
    // providers cleared, upgrade again → still empty
}
TEST_CASE("Upgrading keeps an existing omniroute entry as it is") {
    // v2 with {id "omniroute", openai-compatible, "http://localhost:20128/v1"} → unchanged, size 1
}
TEST_CASE("create asks for a base URL only for HTTP kinds") {
    // registry.registerKind(kKindAgy, factory returning a stub) → create({kind agy, baseUrl ""}) succeeds
    // create({kind openai-compatible, baseUrl ""}).error().code == ErrorCode::InvalidArgument
}
```
Plus `ConfigStoreTest` "ConfigStore round-trips fallback and modelsUrl" (`fallback=false`, `modelsUrl="https://x/models"` survive save/load; a provider without the keys loads `fallback==true`), `OpenAiProviderTest` "listModels asks modelsUrl when one is set" (`FakeHttpServer`, `modelsUrl = server.baseUrl().toString() + "/catalog"`, `requestHead().startsWith("GET /v1/catalog")`), and `VoicePureTest`'s upgrade case rewritten on the `openai` preset (`sttModel "whisper-1"`, `ttsModel "tts-1"`, `ttsVoice "alloy"`, key kept; second provider untouched; a cleared `ttsModel` stays cleared on a second upgrade), calling `upgrade(config, [](const QString &) { return false; })`.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build 2>&1 | tail -5`
Expected: compile errors (`kKindAgy`, `defaultProviders`, `modelsUrl` unknown).

- [ ] **Step 3: Implement**

Presets, in this order: `opencode` ("opencode", no key, empty URL), `agy` ("agy", no key, empty URL), `opencode-console` ("OpenCode Console"), `opencode-console-claude` ("OpenCode Console (Claude)"), `opencode-go` ("OpenCode Go"), `opencode-go-claude` ("OpenCode Go (Claude)"), then the existing OpenAI … Gemini rows unchanged. `upgrade`: for `version < 2` the existing speech-model fill; for `version < 3` and an empty provider list, `defaultProviders(installed)` with the first as `activeProvider`; then `version = kVersion`. `create`: the base-URL check only when `isHttpKind`. `OpenAiProvider`/`AnthropicProvider::listModels`: `QUrl(config_.modelsUrl)` when set. `main.cc`: delete the "providers empty → default" block; when `!QFile::exists(store.path())` set `config.providers = ProviderRegistry::defaultProviders(installed)` and `activeProvider` to the first; call `upgrade(config, installed)` where `installed = [](const QString &p) { return !findProgram(p).isEmpty(); }`. Settings' base-URL placeholder becomes `https://api.example.com/v1`. Reword the listed comments to "a gateway" / "the provider".

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build build && ./build/tests/nexa_tests -# "[#ProviderRegistryTest]" && ctest --test-dir build -j4 2>&1 | tail -3`
Expected: all pass. Then `git grep -il omniroute -- ':!ARCHITECTURE.md' ':!docs'` lists only `CLAUDE.md` and `README.md` (Task 10 rewrites them).

- [ ] **Step 5: Commit** — `git add -A core app tests voice && git commit -m "M15: OmniRoute out; opencode, agy and OpenCode API presets; config v3"`

---

### Task 2: Text-tools replies stream, the markup held back

**Files:**
- Modify: `core/tools/TextToolParser.h/.cc`, `core/agent/Agent.h/.cc`
- Test: `tests/TextToolParserTest.cc`, `tests/AgentTest.cc`

**Interfaces:**
- Produces: `class TextToolParser::StreamFilter { public: QString feed(const QString &delta); const QString &shown() const; };`
  - Never shows leading whitespace. While nothing is shown, a reply whose first non-space character is `{`, `[` or `` ` `` is held entirely. Otherwise text streams up to the first `<tool_call`; a tail that is a proper prefix of `<tool_call>` (`<`, `<t`, …) waits for the next delta; once `<tool_call` is seen, nothing more is shown.
- Agent: in text-tools mode each step's `textDelta` goes through a fresh `StreamFilter`; at the step's end the Agent emits the part of the final visible text (`parsed.text`, or the whole reply when there are no calls) not yet shown, comparing against `shown()` with surrounding whitespace removed.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("StreamFilter passes prose through as it arrives") {
    TextToolParser::StreamFilter f;
    CHECK(f.feed(u"Hello "_s) == u"Hello "_s);
    CHECK(f.feed(u"world"_s) == u"world"_s);
}
TEST_CASE("StreamFilter holds back a call and the partial tag before it") {
    TextToolParser::StreamFilter f;
    CHECK(f.feed(u"Opening it.<to"_s) == u"Opening it."_s);
    CHECK(f.feed(u"ol_call>{\"name\": \"open_app\"}"_s).isEmpty());
    CHECK(f.feed(u"</tool_call>"_s).isEmpty());
}
TEST_CASE("StreamFilter lets a '<' through once it is clearly not a tag") {
    TextToolParser::StreamFilter f;
    CHECK(f.feed(u"x <"_s) == u"x "_s);
    CHECK(f.feed(u"= y"_s) == u"<= y"_s);
}
TEST_CASE("StreamFilter holds a reply that starts like a bare call") {
    TextToolParser::StreamFilter f;
    CHECK(f.feed(u"  {\"name\": \"open_app\""_s).isEmpty());
}
```
AgentTest (give `ScriptedProvider::Reply` a `QStringList chunks`, emitted one `emitText` each before `text`):
- "A text-tools reply streams its prose and never shows the call markup": `nativeTools=false`; first reply chunks `{"Opening the calculator.\n<tool", "_call>{\"name\": \"open_app\", \"arguments\": {}}</tool_call>"}`, second reply `"Done."`; the concatenated `textDelta`s contain `"Opening the calculator."` and `"Done."`, never `"<tool"`, and more than one `textDelta` fired before the first `toolStarted`.
- "A reply that is JSON but not a call is shown in full at the end": `nativeTools=false`, reply `"{\"answer\": 42}"` → concatenated deltas `== "{\"answer\": 42}"`.

- [ ] **Step 2:** `cmake --build build && ./build/tests/nexa_tests "StreamFilter*"` → compile error (no `StreamFilter`).
- [ ] **Step 3:** Implement `StreamFilter` and the Agent change; update the `textTools_` comment in `Agent.h` ("streamed through StreamFilter, the markup held back").
- [ ] **Step 4:** `./build/tests/nexa_tests "StreamFilter*" "A text-tools reply*" "A reply that is JSON*" "A model that cannot call tools*"` → all pass.
- [ ] **Step 5: Commit** — `git commit -am "M15: text-tools replies stream, tool-call markup held back"`

---

### Task 3: TurnLedger and TranscriptWriter

**Files:**
- Create: `core/ai/TurnLedger.h/.cc`, `core/ai/TranscriptWriter.h/.cc`
- Modify: `core/agent/Conversation.h/.cc` (expose the note), `core/CMakeLists.txt`
- Test: create `tests/TurnLedgerTest.cc`; `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
```cpp
// Conversation.h
static QString withoutImageNotes(const QString &text);   // removes "\n[N earlier image(s) omitted]"

// TurnLedger.h
struct TurnPlan {
    bool fresh{true};
    QList<ChatMessage> history;        // fresh only: the messages before the new turns
    QList<ChatMessage> turns;          // the new turns: no system, no assistant; images kept
    QString system;                    // the request's system prompt ("" if none)
    bool instructionsChanged{false};   // continue only: the system prompt differs from the session's
};
class TurnLedger {   // pure
public:
    TurnPlan plan(const ChatRequest &request) const;   // request after TextToolParser::asText
    void commit(const ChatRequest &request);           // the backend took it and answered
    void reset();
    bool isEmpty() const;
};

// TranscriptWriter.h — Builder of the text a backend turn carries (pure, static)
static QString instructions(const QString &system);
    // "[Instructions from Nexa — follow them for the whole conversation]\n\n" + system
static QString instructionsUpdate(const QString &system);
    // "[Nexa's instructions have changed; these replace the earlier ones]\n\n" + system
static QString history(const QList<ChatMessage> &history);
    // "" for none; else "[Earlier in this conversation]\n\nUser: …\n\nNexa: …" (blank line between turns)
static QString turnText(const QList<ChatMessage> &turns);   // texts joined by "\n\n"
```
- `plan` algorithm (messages compared by role and `withoutImageNotes(text).trimmed()`; images ignored):
```text
req = request.messages without a leading System message; system = that message's text
k   = index just after the last Assistant message in req (0 if none)
turns = req[k..]; prefix = req[0..k)
fresh when: ledger empty, request.model != committed model, turns empty, prefix empty,
            or prefix[0..k-1) is not a contiguous tail of the committed messages
otherwise continue; instructionsChanged = system != committed system
fresh plan: history = prefix, turns = turns
commit: committed messages = req, model = request.model, system = system
```

- [ ] **Step 1: Write the failing tests** (`sys`, `u(…)`, `a(…)` helpers build `ChatMessage`s):

```cpp
TEST_CASE("An empty ledger starts fresh with the history before the new turns")   // [sys,u1] → fresh, history {}, turns {u1}
TEST_CASE("After a reply, the next user turn continues")                          // commit [sys,u1]; [sys,u1,a1,u2] → !fresh, turns {u2}, !instructionsChanged
TEST_CASE("Tool results after a call continue the same session")                  // [sys,u1,a("<tool_call>…"),u("<tool_result …>")] → turns {that result}
TEST_CASE("The assistant's own words never have to match")                        // a1 text "anything" → continue
TEST_CASE("A trimmed history still continues")                                     // commit [sys,u1,a1,u2]; [sys,u2,a2,u3] → continue, turns {u3}
TEST_CASE("An older screenshot turned into a note still matches")                 // commit [sys,u("look", 1 image)]; [sys,u("look\n[1 earlier image(s) omitted]"),a1,u2] → continue
TEST_CASE("A different model, a new conversation or edited history starts fresh") // model "m2" → fresh with history {u1,a1}; [sys,u9] → fresh; u1 text changed → fresh
TEST_CASE("Changed instructions are reported")                                    // system "B" after "A" → continue, instructionsChanged
TEST_CASE("reset forgets the session")
TEST_CASE("TranscriptWriter labels each earlier turn")
    // history({u("hi"), a("hello")}) == "[Earlier in this conversation]\n\nUser: hi\n\nNexa: hello"; history({}) == ""
```
- [ ] **Step 2:** build → compile errors (no `TurnLedger`).
- [ ] **Step 3:** Implement; `Conversation` uses the same pattern when it writes the note.
- [ ] **Step 4:** `./build/tests/nexa_tests -# "[#TurnLedgerTest]" "[#ConversationTest]"` → pass.
- [ ] **Step 5: Commit** — `git commit -am "M15: TurnLedger and TranscriptWriter"` (with the new files added)

---

### Task 4: AgyEvents — agy's NDJSON, pure

**Files:**
- Create: `core/ai/AgyEvents.h/.cc`; Test: `tests/AgyEventsTest.cc`; both CMakeLists

**Interfaces:**
- Produces:
```cpp
struct AgyEvent {
    enum class Kind { Init, Text, ToolStep, Result, Ignored };
    Kind kind{Kind::Ignored};
    QString text;              // Text: step_type agent_response, its text_delta
    QString toolName;          // ToolStep
    bool ok{true};             // Result: status == "SUCCESS"; ToolStep: state != "ERROR"
    QString response;          // Result
    QString error;             // Result
    QStringList deniedActions; // Result: denied_actions[].display_name
};
class AgyEvents {
public:
    static AgyEvent decode(const QByteArray &line);
    static QByteArray userTurn(const QString &text);   // {"event":"user","message":{"content":[{"type":"text","text":…}]}}\n
    static QStringList parseModels(const QString &output);   // ids from "id\tname" lines; other lines skipped
    static Error exitError(int exitCode, const QString &stderrTail);
        // "authenticat" in stderr → Unauthorized "agy is not signed in — run agy once in a terminal to sign in";
        // otherwise ConnectionRefused "agy stopped (exit N): <last stderr line>"
    static Error resultError(const QString &error);
        // "quota", "exhausted", "rate limit" or "429" in it → Http "HTTP 429: agy: <error>"; otherwise Unknown "agy: <error>"
};
```

- [ ] **Step 1: Write the failing tests** with the lines measured on 2026-10-27 (spec §2):
  - "decode reads agy's init, text, tool and result events": the four lines from the spec, checking `Kind`, `text == "OK"`, `toolName == "run_command"` with `ok == false` for `state:"ERROR"`, result `ok`, `response == "OK\n"`.
  - "decode reports denied actions and errors": `{"event":"result","result":{"status":"SUCCESS","response":"","denied_actions":[{"action":"command","display_name":"RunCommand"}]}}` → `deniedActions == {"RunCommand"}`; `status:"ERROR","error":"stream input message is missing the \"event\" field"` → `!ok`, that error.
  - "decode ignores what it does not know": `""`, `"warning: ignoring …"`, `{"event":"step_update","step_update":{"step_type":"user_input","state":"DONE"}}` → `Ignored`.
  - "userTurn writes one line agy accepts": parses back to the object above, ends with `'\n'`, a text with quotes and a newline round-trips.
  - "parseModels reads agy models' output": `"Fetching available models...\ngemini-3.8-flash-high\tGemini 3.8 Flash (High)\nclaude-sonnet-4-6\tClaude Sonnet 4.6 (Thinking)\n"` → `{"gemini-3.8-flash-high","claude-sonnet-4-6"}`.
  - "exitError and resultError say what to do": the two mappings above.
- [ ] **Step 2:** build fails; **Step 3:** implement; **Step 4:** `./build/tests/nexa_tests -# "[#AgyEventsTest]"` passes.
- [ ] **Step 5: Commit** — `git commit -m "M15: AgyEvents, agy's stream-json decoded"` (files added)

---

### Task 5: AgySession and AgyProvider, with a stand-in agy

**Files:**
- Create: `core/ai/AgySession.h/.cc`, `core/ai/AgyProvider.h/.cc`, `tests/fake-agy/agy` (Python 3, executable), `tests/AgyProviderTest.cc`
- Modify: `core/ai/IAiProvider.h` (add `virtual void warmUp() {}` — "start whatever makes the first answer slow; default nothing"), `core/ai/ProviderRegistry.cc` (register `kKindAgy`), both CMakeLists

**Interfaces:**
- Consumes: `AgyEvents` (Task 4), `TurnLedger`, `TranscriptWriter` (Task 3), `findProgram` (Task 1).
- Produces:
```cpp
class AgySession : public QObject {          // one warm agy process, one turn at a time
    Q_OBJECT
public:
    AgySession(QString program, QString model, QString workingDir, QObject *parent = nullptr);
    ~AgySession() override;                  // kills the process group
    void start();                            // idempotent
    void send(const QString &text);          // one turn: AgyEvents::userTurn(text)
    void stop();
    bool isUsable() const;                   // started or starting, not broken
    bool isBusy() const;
    int turnsTaken() const;
    const QString &model() const;
signals:
    void text(const QString &delta);
    void toolStep(const QString &tool, bool allowed);
    void turnFinished(const nexa::AgyEvent &result);
    void turnFailed(const nexa::Error &error);   // process failed to start or exited mid-turn (AgyEvents::exitError)
};
class AgyProvider : public IAiProvider {   // Strategy
public:
    explicit AgyProvider(ProviderConfig config, QObject *parent = nullptr);
    Capabilities capabilities() const override;   // {.streaming = true, .nativeTools = false, .vision = false}
    void listModels(std::function<void(Result<QStringList>)> done) override;   // `agy models`, 30 s, cached
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;
    void warmUp() override;                       // starts a session for config.defaultModel
};
```
- chat: program = `findProgram("agy", config.baseUrl)` (empty → stream fails `NotFound` "agy is not installed (looked on PATH and in ~/.local/bin)"). Model = `request.model`, else `config.defaultModel`. A fresh plan reuses the current session only if it is usable, idle, `turnsTaken() == 0` and the model matches; otherwise it replaces it. Fresh text = `instructions(system)` + `"\n\n"` + `history(plan.history)` (when non-empty, + `"\n\n"`) + `turnText(turns)`; continue text = (`instructionsUpdate(system)` + `"\n\n"` when changed) + `turnText(turns)`. `text` → `emitText`; `turnFinished` ok with non-empty response → `complete(Stop)` and `ledger.commit(request)`; ok, empty response, `deniedActions` non-empty, not yet reminded → send `"Use only Nexa's tools, written as <tool_call> blocks as described in Nexa's instructions. Your own tools are switched off."`; the same again → fail `NotSupported` "agy tried to use its own %1 tool, which Nexa does not allow" (first denied action); `!ok` → fail `AgyEvents::resultError`; a failed or cancelled turn → `ledger.reset()`; cancel → `session->stop()`. A second `chat` while busy fails `Unknown` "agy is still answering the previous request". Idle 10 min → `stop()`.
- `tests/fake-agy/agy`: `agy models` prints `Fetching available models...` then `fake-fast\tFake Fast` and `fake-smart\tFake Smart`; otherwise it appends `{"argv": […]}` to `$FAKE_AGY_LOG` at start, then for each stdin line appends `{"turn": "<text>"}` and plays the next entry of the JSON list in `$FAKE_AGY_SCRIPT` (`{"text": "…"}` as two `text_delta` steps and a SUCCESS result; `{"deny": "RunCommand"}` as an empty SUCCESS with that denied action; `{"error": "…"}` as an ERROR result; `{"die": "not authenticated"}` writes that to stderr and exits 1).

- [ ] **Step 1: Write the failing tests** (`AgyProviderTest.cc`; `config.baseUrl = NEXA_TESTS_DIR "/fake-agy/agy"`, env vars set per test with `qputenv`, waits with `QTRY_*` or a `QSignalSpy` up to 5 s):
  - "AgyProvider lists the models agy prints" → `{"fake-fast","fake-smart"}`.
  - "A first request starts agy with Nexa's instructions and streams the reply": argv contains `--print=`, `--input-format`, `--model`, `fake-fast`; the logged turn starts with `[Instructions from Nexa` and ends with the user's text; streamed text equals the scripted reply; `capabilities().nativeTools == false`.
  - "The next request reuses the warm process and sends only the new turn": one `argv` entry in the log; the second turn is exactly the new user text.
  - "agy reaching for its own tool gets one reminder, then the step fails with the tool's name": script `[{"deny":"RunCommand"},{"deny":"RunCommand"}]` → second logged turn starts with `Use only Nexa's tools`; failure `NotSupported` containing `RunCommand`.
  - "A process that dies before answering reports why": `{"die":"not authenticated"}` → `Unauthorized`, message contains `run agy once`.
  - "A missing program is NotFound": `baseUrl = "/nonexistent/agy"`.
  - "Cancel kills the process and the next request starts fresh": cancel mid-turn (script entry `{"hang": true}` — sleeps), then a request → two `argv` entries; the earlier process is gone (`kill(pid, 0)` fails — the fake logs its pid).
- [ ] **Step 2:** build fails. **Step 3:** implement; register `kKindAgy` in `ProviderRegistry`'s constructor. **Step 4:** `./build/tests/nexa_tests -# "[#AgyProviderTest]"` passes, and the whole suite.
- [ ] **Step 5: Check two things against the real agy** (scratch folder `/tmp/nexa-agy-check`, cheapest model `gemini-3.8-flash-low`):
  - *Read tools*: a warm turn "Use your view_file tool to read ~/.bashrc and tell me its first line." If the result shows `view_file` running without a denial, stop and report to the owner before going on (the spec's §8 open item) — do not proceed silently.
  - *Images*: send one turn with a small PNG as `{"type":"image","source":{"type":"base64","media_type":"image/png","data":…}}`, then `{"type":"image","mime_type":"image/png","data":…}`. If one is accepted and the model describes the picture, add `images` to `AgyEvents::userTurn` in that shape, set `vision = true`, and add an `AgyEventsTest` case for it; otherwise leave `vision = false`.
- [ ] **Step 6: Commit** — `git commit -m "M15: agy pipeline — a warm agy process per conversation"` (files added)

---

### Task 6: HttpClient::getStreaming and OpenCodeWire

**Files:**
- Modify: `core/net/HttpClient.h/.cc`
- Create: `core/ai/OpenCodeWire.h/.cc`, `tests/OpenCodeWireTest.cc`; both CMakeLists

**Interfaces:**
- Produces:
```cpp
QNetworkReply *HttpClient::getStreaming(const QNetworkRequest &request, int idleTimeoutMs);   // as postStreaming, GET

struct OpenCodeEvent {
    enum class Kind { TextDelta, Busy, Idle, Retry, Error, Other };
    Kind kind{Kind::Other};
    QString sessionId;
    QString text;          // TextDelta (field "text" only; "reasoning" is Other)
    QString message;       // Retry / Error
    QString reason;        // Retry: action.reason
    bool authError{false}; // Error: error.name == "ProviderAuthError"
};
class OpenCodeWire {   // pure
public:
    static QJsonObject promptBody(const QString &model, const QString &system, const QString &text,
                                  const QList<ImageAttachment> &images);
        // {"model":{"providerID","modelID"}} split at the first '/', omitted when model is empty;
        // "system" when non-empty; "tools":{"*":false}; "parts":[{"type":"text","text"}, {"type":"file","mime","url":"data:<mime>;base64,…"}…]
    static OpenCodeEvent decodeEvent(const QByteArray &data);
    static QStringList parseModels(const QByteArray &configProviders, const QString &preferred);
        // "providerID/modelID" for every model, sorted; `preferred` moved first when listed
    static QString parseDefaultModel(const QByteArray &config);   // GET /config → "model", or ""
    static QUrl parseListening(const QByteArray &output);           // "opencode server listening on http://…" → that URL
    static QString parseSessionId(const QByteArray &body);          // POST /session → "id"
    static QNetworkRequest request(const QUrl &base, const QString &path, const QString &password);
        // JSON content type, Authorization "Basic " + base64("opencode:" + password)
};
```

- [ ] **Step 1: Write the failing tests** with the payloads recorded in the spec §2:
  - "promptBody carries Nexa's system prompt with opencode's tools off": `promptBody("google/antigravity-gemini-3-flash", "S", "hi", {})` → `model.providerID == "google"`, `model.modelID == "antigravity-gemini-3-flash"`, `system == "S"`, `tools == {"*": false}`, one text part; a model `"openrouter/meta/llama"` splits at the first `/` only; empty model → no `model` key; one image → a `file` part with `url` starting `data:image/png;base64,`.
  - "decodeEvent reads deltas, status, idle and errors": `message.part.delta` with `field:"text"` → `TextDelta` and the delta; `field:"reasoning"` → `Other`; `session.status` busy → `Busy`; the measured retry payload (`"attempt":1,"message":"Free usage exceeded, subscribe to Go","action":{"reason":"free_tier_limit"}`) → `Retry`, `reason == "free_tier_limit"`; `session.idle` → `Idle` with the session id; `session.error` with `{"name":"ProviderAuthError","data":{"message":"x"}}` → `Error`, `authError`, `message == "x"`.
  - "parseModels lists every provider's models with the default first".
  - "parseListening finds the server's address" (`"opencode server listening on http://127.0.0.1:4096\n"` → that URL; `"[auto-update-checker] …"` alone → invalid URL).
  - "request authenticates as opencode" (header equals `"Basic " + QByteArray("opencode:pw").toBase64()`).
- [ ] **Step 2:** build fails. **Step 3:** implement. **Step 4:** `./build/tests/nexa_tests -# "[#OpenCodeWireTest]"` passes.
- [ ] **Step 5: Commit** — `git commit -m "M15: OpenCodeWire and a streaming GET"` (files added)

---

### Task 7: OpenCodeServer and OpenCodeProvider, with a stand-in opencode

**Files:**
- Create: `core/ai/OpenCodeServer.h/.cc`, `core/ai/OpenCodeProvider.h/.cc`, `tests/fake-opencode/opencode` (Python 3, executable), `tests/fake-opencode/silent` (executable: `sleep 30`), `tests/OpenCodeProviderTest.cc`
- Modify: `core/ai/ProviderRegistry.h/.cc` (own the servers, register `kKindOpenCode`), both CMakeLists

**Interfaces:**
- Consumes: `OpenCodeWire`, `getStreaming` (Task 6), `TurnLedger`, `TranscriptWriter` (Task 3), `findProgram` (Task 1), `SseParser`.
- Produces:
```cpp
class OpenCodeServer : public QObject {   // Facade over Nexa's private `opencode serve`, or one to attach to
    Q_OBJECT
public:
    OpenCodeServer(HttpClient *http, QString program, QUrl attachUrl, QString attachPassword, QObject *parent = nullptr);
    ~OpenCodeServer() override;                       // stops a server Nexa started (process group)
    void ensureStarted(std::function<void(Result<void>)> done);   // calls queue while starting
    QNetworkRequest request(const QString &path) const;
    bool isReady() const;
    void setStartTimeoutMs(int ms);                   // 30'000; tests use less
    qint64 processId() const;                         // 0 when attached or not running
signals:
    void event(const nexa::OpenCodeEvent &event);     // the shared /event stream
};
class OpenCodeProvider : public IAiProvider {   // Strategy
public:
    OpenCodeProvider(ProviderConfig config, HttpClient *http, std::shared_ptr<OpenCodeServer> server,
                     QObject *parent = nullptr);
    Capabilities capabilities() const override;   // {.streaming = true, .nativeTools = false, .vision = true}
    void listModels(std::function<void(Result<QStringList>)> done) override;   // GET /config + /config/providers
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;
    void warmUp() override;                       // server->ensureStarted
};
// ProviderRegistry: std::shared_ptr<OpenCodeServer> openCodeServer(const ProviderConfig &config);
//   one per (program, URL), kept for the registry's life so a rebuilt provider finds its server running
```
- Start: program `findProgram("opencode", "")` (missing → `NotFound` "opencode is not installed (looked on PATH and in ~/.local/bin)"); args `serve --hostname 127.0.0.1 --port 0`; environment adds `OPENCODE_SERVER_PASSWORD` (random) and `OPENCODE_CONFIG_CONTENT={"permission":{"edit":"deny","bash":"deny","webfetch":"deny"}}`; ready = `parseListening` URL seen on stdout and `GET /global/health` 200 (polled every 250 ms); over the limit → `Timeout` "opencode did not start within 30 s". Then one `/event` stream (`getStreaming`, idle 60 s), reopened 1 s after it drops while the process lives. Attach mode (`config.baseUrl` set): no process, `attachPassword = config.apiKey`.
- Turn: fresh → `DELETE` the previous session (best effort), `POST /session {"title":"Nexa"}`; text = `history(plan.history)` (+ `"\n\n"`) + `turnText(turns)`; continue → `turnText(turns)`. `POST /session/{id}/prompt_async` with `promptBody(model, system, text, images of turns)`. The turn ends at the first `Idle` for the session after a `Busy` for it (→ `complete(Stop)`, `ledger.commit`). `Error` → fail (`authError` → `Unauthorized` "opencode is not signed in for this model — run `opencode auth login` in a terminal"; else `Http` "opencode: <message>"). `Retry` whose reason or message contains `limit`, `quota` or `credit`, or any retry more than 30 s after the first → `POST …/abort`, fail `Http` "HTTP 429: opencode: <message>". Cancel → `POST …/abort`, `ledger.reset()`.
- `tests/fake-opencode/opencode`: `serve --hostname H --port 0` binds a free port, prints `opencode server listening on http://127.0.0.1:<port>`, answers 401 unless the basic-auth password equals `$OPENCODE_SERVER_PASSWORD`; routes `GET /global/health`, `GET /config` (`{"model":"fake/smart"}`), `GET /config/providers` (providers `fake` with `fast`, `smart` and `other` with `one`), `POST /session` (`{"id":"ses_<n>"}`), `POST /session/{id}/prompt_async` (204; then plays the next entry of `$FAKE_OPENCODE_SCRIPT` on `/event`: `{"text":"…"}` → busy, two deltas, idle; `{"retry":"Free usage exceeded, subscribe to Go","reason":"free_tier_limit"}`; `{"autherror":"x"}`; `{"hang":true}`), `POST /session/{id}/abort`, `DELETE /session/{id}`, `GET /event` (SSE, heartbeats). Every request is appended to `$FAKE_OPENCODE_LOG` as `{"method","path","body"}`; its pid is logged at start.

- [ ] **Step 1: Write the failing tests** (`OpenCodeProviderTest.cc`, the fake as `program`, start limit 3 s in the slow test):
  - "Nexa starts its own opencode server with a password and lists its models, the default first" → `models.first() == "fake/smart"`, contains `"other/one"`.
  - "A request streams the reply with Nexa's system prompt and opencode's tools off" → logged prompt body: `system` equals the request's system text, `tools == {"*":false}`, `model == {"providerID":"fake","modelID":"fast"}`; streamed text equals the script's.
  - "The next request continues the same session and sends only the new turn" → one `POST /session`; the second prompt's text part equals the new user text.
  - "A quota retry gives up at once and says so" → failure `Http`, message starts with `HTTP 429: opencode:`; the log has `POST /session/ses_1/abort`.
  - "An auth error tells the user to sign in" → `Unauthorized`, message contains `opencode auth login`.
  - "Cancel aborts the session".
  - "A server that never says it is listening fails within the limit" (program `tests/fake-opencode/silent`, limit 1500 ms) → `Timeout`.
  - "Attach mode uses the given URL and password and starts nothing" (start the fake by hand with `QProcess`, provider `baseUrl` = its URL, `apiKey` = its password) → models listed, `processId() == 0`.
  - "Destroying the server stops the process" → after the server object is destroyed, `kill(pid, 0) != 0`.
- [ ] **Step 2:** build fails. **Step 3:** implement; register `kKindOpenCode` with `openCodeServer(config)`. **Step 4:** `./build/tests/nexa_tests -# "[#OpenCodeProviderTest]"` and the whole suite pass.
- [ ] **Step 5: Commit** — `git commit -m "M15: opencode pipeline — Nexa's own opencode server, one session per conversation"` (files added)

---

### Task 8: ProviderChain

**Files:**
- Create: `core/ai/ProviderChain.h/.cc`, `tests/ProviderChainTest.cc`
- Modify: `core/ai/ChatStream.h/.cc`, `core/ai/ProviderRegistry.h/.cc`, both CMakeLists

**Interfaces:**
- Produces:
```cpp
// ChatStream: who answered when it was not the first choice
const QString &route() const;                                     // "" for the first choice
signals: void rerouted(const QString &route, const QString &reason);
protected: void setRoute(const QString &route, const QString &reason);   // emits rerouted

class ProviderChain : public IAiProvider {   // Chain of Responsibility, as RecognizerChain
    Q_OBJECT
public:
    struct Link { std::unique_ptr<IAiProvider> provider; QString model; };   // the provider's own model
    explicit ProviderChain(std::vector<Link> links, QObject *parent = nullptr);
    QString id() const override;  QString displayName() const override;     // the first link's
    Capabilities capabilities() const override;   // the first link not in its skip window
    void listModels(std::function<void(Result<QStringList>)> done) override; // the first link's
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;
    void warmUp() override;                                                  // the first link's
    void setSkipWindowMs(int ms);                                            // 60'000
    static bool meansUnavailable(const Error &error);
        // ConnectionRefused, NotFound, Unauthorized, Timeout, Network; Http whose leading status
        // ("HTTP 429: …", "[503]: …") is 429 or 5xx. Nothing else.
    static ChatRequest asTextRequest(const ChatRequest &request);
        // messages through TextToolParser::asText, TextToolParser::instructions(request.tools) appended to
        // (or prepended as) the system message, tools cleared
};
// ProviderRegistry:
Result<std::unique_ptr<IAiProvider>> createChain(const Config &config) const;
    // links: config.active() first, then every other provider with fallback, in list order; a provider that
    // cannot be created is left out (logged); none → the first error
```
- Link request: the first link uses `request.model` (its own model when empty); later links their `Link::model`. A link whose `capabilities().nativeTools` is false gets `asTextRequest(request)` when `request.tools` is non-empty. The stream forwards text, tool deltas, finished and failed; on a failure with no output yet, `meansUnavailable`, and a next link: the failed link is skipped for the window, the next one tried, `setRoute("<displayName> · <model>", "<failed name>: <message>")`. A link that answers clears its skip. All links failed → `failed(Error{last code, "<name>: <message>; <name>: <message>"})`. `cancel()` cancels the inner stream.

- [ ] **Step 1: Write the failing tests** (`ProviderChainTest.cc` with a small scripted fake provider per link: a name, `nativeTools`, and a queue of outcomes — text, or an `Error` — recording each `ChatRequest`):
  - "The first link answers when it can" → `route()` empty, no `rerouted`.
  - "An unreachable first link hands the request to the next, which uses its own model" → link 1 `ConnectionRefused`; link 2 got `model == "m2"`; `rerouted("Two · m2", reason contains "One:")`.
  - "A failure after text has streamed is not retried elsewhere" → link 1 emits text then `ConnectionRefused` → `failed`; link 2 never called.
  - "A 400 refusal is passed through, a 429 moves on" → `Http "HTTP 400: image"` fails; `Http "HTTP 429: busy"` reroutes.
  - "A link that failed is skipped for the window, then tried again" (window 50 ms).
  - "When every link fails, one message names each" → contains `"One: "` and `"; Two: "`.
  - "A native request falling to a text-only link is converted" → link 2 (`nativeTools=false`) receives no tools, a system message containing `<tool_call>`.
  - "createChain puts the first choice first and leaves out providers without fallback" (registry with a stub kind registered; providers A, B(fallback=false), C, active C → link order C, A).
- [ ] **Step 2:** build fails. **Step 3:** implement. **Step 4:** `./build/tests/nexa_tests -# "[#ProviderChainTest]"` passes.
- [ ] **Step 5: Commit** — `git commit -m "M15: ProviderChain — the next provider when one cannot answer"` (files added)

---

### Task 9: The app — the chain in use, "via" in the chat, Settings → AI

**Files:**
- Modify: `core/agent/Agent.h/.cc` (signal `rerouted(const QString &route, const QString &reason)`, forwarded from each step's stream), `app/AppController.h/.cc`, `app/ChatView.h/.cc`, `app/MainWindow.cc`, `app/SettingsDialog.h/.cc`
- Test: `tests/AgentTest.cc`

**Interfaces:**
- Consumes: `ProviderRegistry::createChain` (Task 8), `IAiProvider::warmUp` (Task 5), `ChatStream::rerouted` (Task 8), `ProviderRegistry::isHttpKind` (Task 1).
- Produces: `AppController` signal `void replyVia(const QString &route);`; `ChatView::setReplyVia(const QString &route)` (cleared by `beginAssistantMessage`; `replyDetail()` appends `" · via " + route` to the agent detail).
- `AppController::rebuildProvider` builds `services_.providers->createChain(config_)`; `start()` calls `provider_->warmUp()` after building it. On the Agent's `rerouted`: `emit replyVia(route)` and `statusChanged(tr("AI: %1 — %2").arg(route, reason), false)`.
- Settings → AI: "Active provider:" → **"First choice:"**; ▲ / ▼ buttons beside Add/Remove move the selected provider in `config_.providers` and the list; new check box `fallbackCheck_` "Use when the providers above cannot answer" stored in `ProviderConfig::fallback`; rows shown per kind with `QFormLayout::setRowVisible`: HTTP kinds — everything as today; `agy` — the URL row labelled "Program:" (placeholder "agy on PATH"), no key, speech or "cannot call tools" rows; `opencode` — "Server:" (placeholder "Nexa starts its own"), key row labelled "Server password:" (placeholder "only for a server you run yourself"), no speech or tools rows.

- [ ] **Step 1: Write the failing test** — AgentTest "A fallback that answered is reported": a scripted stream that calls `setRoute("Two · m2", "One: down")` before its text → the Agent emits `rerouted("Two · m2", "One: down")` once.
- [ ] **Step 2:** build fails. **Step 3:** implement the Agent signal, the controller, the chat header and Settings.
- [ ] **Step 4:** `ctest --test-dir build -j4` all pass; then look at the screens: `QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME=/tmp/nexa-m15/config XDG_DATA_HOME=/tmp/nexa-m15/data ./build/app/nexa-assistant --new-instance --screenshot-settings /tmp/nexa-m15/settings` (with `playwright`, `models`, `piper` symlinked into `/tmp/nexa-m15/data/nexa` as CLAUDE.md describes) and read the AI tab's picture: the list shows opencode and agy, "First choice: opencode", ▲/▼ present, the opencode form shows Server/Server password and the fallback box, no speech rows.
- [ ] **Step 5: Commit** — `git commit -am "M15: the chain in the app — via in the chat, First choice and order in Settings"`

---

### Task 10: Live verification and the documents

**Files:**
- Modify: `ARCHITECTURE.md` (Status row "M15 — agy and opencode pipelines, the chain, OmniRoute removed" with what was verified; Deviations entries; "Decisions made by the user": OmniRoute line replaced by "AI (M15): the owner's agy and opencode, through the programs themselves; an ordered chain; OmniRoute removed"; "AI providers" section rewritten), `CLAUDE.md` (runtime paragraph: agy/opencode sign-in, Nexa's own opencode server and its `~/.cache/nexa` folders, OpenCode API presets; Layout: the new `core/ai` files; voice sentence without OmniRoute), `README.md` (providers)

- [ ] **Step 1: Live runs**, each a separate copy on private folders (`/tmp/nexa-m15/{config,data}`, the three symlinks), `QT_QPA_PLATFORM=offscreen`, `QT_LOGGING_RULES="nexa.ai.debug=true"`, `--new-instance`:
  1. First start writes opencode and agy into the private config (check `config.json`: version 3, both providers).
  2. `--ask "what is 2+2" --screenshot /tmp/nexa-m15/oc.png` → an answer streamed through opencode (log: one `opencode serve` start, one session).
  3. `--ask "open calculator and tell me the volume"` → `open_app` and `get_volume` ran (Actions, `audit.jsonl` in the private data folder), the answer names the volume.
  4. agy first (Settings order or edit the private config): the same two asks; the log shows one agy start for both steps of the second ask.
  5. opencode first with its `baseUrl` set to `http://127.0.0.1:9` (nothing listens) → the reply's header in the screenshot reads `via agy`.
  6. Settings → Test connection for each provider (screenshot of the AI tab) reports the model count.
  Record each result (pass/fail, times) for the Status row; anything failing goes back to its task.
- [ ] **Step 2:** Update the three documents; `git grep -il omniroute -- ':!ARCHITECTURE.md' ':!docs'` prints nothing.
- [ ] **Step 3:** Full check: `cmake --build build && ctest --test-dir build --output-on-failure -j4` (all pass), `cmake --preset debug && cmake --build build-debug` (no warnings, `-Werror`), and the new tests under ASan: `cmake --preset asan && cmake --build build-asan && ASAN_OPTIONS=detect_leaks=0 ./build-asan/tests/nexa_tests -# "[#TurnLedgerTest],[#AgyEventsTest],[#AgyProviderTest],[#OpenCodeWireTest],[#OpenCodeProviderTest],[#ProviderChainTest]"`.
- [ ] **Step 4: Commit** — `git commit -am "M15: documents and live verification"`
