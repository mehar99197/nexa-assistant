# M16 Voice — a wake name of the owner's choosing, heard instantly; requests heard right — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Nexa wakes to a name the owner picks, hears it instantly and mid-sentence even with a ceiling fan on, takes
the request after it through Groq, allows follow-ups, and lets the owner choose the cloud voice in Settings.

**Architecture:** A *listening front* (noise reduction → voice detector + keyword spotter, on a worker thread) tells
`VoicePipeline` where speech starts and ends and where the name was said; the pipeline cuts the raw audio from a ring
buffer and sends the request to the recogniser chain. The sherpa-onnx library (spotter, Silero VAD, GTCRN denoiser) is
loaded at run time from `~/.local/share/nexa/sherpa`; without it, the energy VAD and the whisper second net work alone.

**Tech Stack:** C++20, Qt 6 (Core, Multimedia, Widgets), whisper.cpp (in the build), sherpa-onnx v2.0.1 C API through
`QLibrary`, Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-10-29-voice-wake-name-design.md` (read it with this plan).

## Global Constraints

- Everything in CLAUDE.md's *Conventions*: `Result<T>`, no exceptions, `u"…"_s`, files listed in `CMakeLists.txt`,
  pattern named in each class comment, nothing heavy on the GUI thread, `core` without QtWidgets.
- **Privacy**: while the wake word is on, only offline recognition (the spotter, whisper) hears the room. Audio goes to
  a cloud recogniser only for: the request said with the name (cut from 0.3 s before the spotter's phrase start, or
  0.2 s before the second net's first word — never earlier), a follow-up in the window, push-to-talk.
- Nexa changes the system input volume **only** when the owner presses **Lower it**.
- Pinned downloads (SHA-256): `sherpa-onnx-v2.0.1-linux-x64-shared-lib.tar.bz2`
  `532c45bbd41036d83cd70606797529aa8086c2053395a58967736c727625b120`;
  `sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2`
  `f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a`; `silero_vad.onnx`
  `9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6`; `gtcrn_simple.onnx`
  `e77603ac0c23dac3227dd2d7135b3a585cbee2679048aecfa886657d3ae1b534`.
- Install layout: `~/.local/share/nexa/sherpa/lib/{libsherpa-onnx-c-api.so,libonnxruntime.so}`,
  `sherpa/kws/{encoder,decoder,joiner}-epoch-12-avg-2-chunk-16-left-64.int8.onnx`, `sherpa/kws/tokens.txt`,
  `sherpa/kws/bpe.model`, `sherpa/silero_vad.onnx`, `sherpa/gtcrn_simple.onnx`.
- Defaults: `wakeName` "Nexa", `wakeSensitivity` "normal", `endPause` "normal", `followUps` true, `noiseReduction` true,
  `sttProvider` "", `ttsProvider` "".
- Sensitivity: Low = spotter boost 1.0 / threshold 0.25 / Silero 0.6 / energy start margin 16 dB; Normal = 2.0 / 0.10 /
  0.5 / 13 dB; High = 3.0 / 0.05 / 0.4 / 10 dB. End pause: short 900 ms, normal 1400 ms, long 2000 ms.
- Timings: second-net segmentation pause 700 ms, longest wake utterance 8 s; name alone → wait 8 s for the request;
  request cap 15 s after the name or in a follow-up, 60 s for push-to-talk; follow-up window 8 s from 150 ms after the
  speech ended, level within 8 dB of the last request; self-guard 3 s; a bare-name spotter hit counts only when the
  utterance it is in started at most 500 ms before the name (the name opens what was said).
- Request prompt, exactly: `Hey ‹Name›. Ammi ko call karo. Volume kam karo. YouTube par Pehchan OST chalao.`
- Copy, exactly: hint `Say “Hey %1” and your request, type one, or hold Talk.`; phone and web `The PC is listening for
  its name.`; wake checkbox `Listen for my wake name — the microphone stays on; only offline recognition hears the
  room`; spotter line `Instant spotting: installed` / `Instant spotting: not installed — run scripts/download-sherpa.sh`;
  noise checkbox `Reduce background noise (uses about a fifth of one CPU core while listening)`.
- Build and test: `cmake --build build-debug --target nexa_tests && ./build-debug/tests/nexa_tests "<name>"`; the whole
  suite `./build-debug/tests/nexa_tests`; `-Werror` on the full `cmake --build build-debug`; the owner runs
  `cmake --build build`.

## Review Focus

1. **The name is changed while the spotter is listening** — a hit from the previous name must not wake Nexa after the
   new one applies. Test in Task 10: "A hit from a replaced listening front is ignored".
2. **A name in Urdu script, or with digits** — Settings refuses it with the reason instead of building a spotter that
   can never fire. Test in Task 3: `check(u"نیکسا")` says Latin letters, `check(u"N3")` says two letters.
3. **The microphone fails while the wake word listens** — a notice, the pipeline idle, the front reset, no crash. Test
   in Task 7: "A failing microphone leaves the pipeline idle and the front reset".
4. **Talk is pressed while a request after the name is being captured, or in the follow-up window** — the key wins and
   exactly one request is made. Tests in Task 10 and Task 11.
5. **Groq hears only the name** ("Hey Nexa.") — Nexa keeps listening for the request instead of saying it heard
   nothing. Test in Task 9: "Only the name back from the recogniser: Nexa waits for the request".

## File Structure

- `voice/wake/` — `WordPieces` (SentencePiece unigram split), `WakePhrases` (spellings, keyword lines, tuning, name
  check), `IWakeSpotter`, `KeywordSpotter`, `SherpaRuntime` (the library loaded at run time), `sherpa/c-api.h` +
  `sherpa/LICENSE` (vendored v2.0.1, Apache-2.0).
- `voice/listen/` — `AudioRing` (raw audio by sample position), `IVoiceDetector`, `EnergyVoiceDetector`,
  `SileroVoiceDetector`, `IAudioFilter`, `NoiseReducer`, `ListeningFront` (Facade, inline or worker thread).
- `voice/capture/` — `MicrophoneCheck` (pure), `RecordedAudioInput` (`--hear`).
- `voice/tts/CloudVoices` — model filters and voice names (pure).
- Changed: `core/config/Config`, `voice/pipeline/WakeWordDetector`, `voice/pipeline/VoicePipeline`,
  `voice/stt/ISpeechRecognizer`, `voice/stt/WhisperRecognizer`, `voice/capture/EnergyVad`, `voice/VoiceBackends`,
  `core/intent/IntentRouter`, `core/agent/VoicePrompt` (new, pure), `system/audio/WpctlAudioController`,
  `app/MicrophoneLeveler` (new), `app/AppController`, `app/SettingsDialog`, `app/SystemStatus.h`, `app/MainWindow`,
  `app/RemoteBridge`, `app/main.cc`, `android/PhoneWindow.cc`, `remote/web/app.js`, `scripts/download-sherpa.sh` (new),
  `README.md`, `CLAUDE.md`, `ARCHITECTURE.md`.
- Tests (new): `WordPiecesTest.cc`, `WakePhrasesTest.cc`, `MicrophoneCheckTest.cc`, `VoiceBackendsTest.cc`,
  `SherpaTest.cc`, `ListeningFrontTest.cc`; changed: `VoicePureTest.cc`, `VoicePipelineTest.cc`, `ConfigStoreTest.cc`,
  `IntentRouterTest.cc`.

---

### Task 1: The new voice settings in the config

**Files:**
- Modify: `core/config/Config.h` (`VoiceConfig`), `core/config/Config.cc` (`VoiceConfig::toJson/fromJson`)
- Test: `tests/ConfigStoreTest.cc`

**Interfaces:**
- Produces: `VoiceConfig::wakeName` (QString), `wakeSensitivity` (QString), `endPause` (QString), `followUps` (bool),
  `noiseReduction` (bool), `sttProvider` (QString), `ttsProvider` (QString) — JSON keys of the same names.

- [ ] **Step 1: Write the failing test** `TEST_CASE("Voice settings for the wake name, listening and the cloud voice round-trip")`
  - `VoiceConfig{}` has the Global Constraints defaults.
  - A config with `wakeName "Jarvis"`, `wakeSensitivity "high"`, `endPause "long"`, `followUps false`,
    `noiseReduction false`, `sttProvider "groq"`, `ttsProvider "groq"` saves and loads equal.
  - `VoiceConfig::fromJson({{"wakeSensitivity","loud"},{"endPause","forever"},{"wakeName",""}})` gives `"normal"`,
    `"normal"`, `"Nexa"`.
- [ ] **Step 2: Run it** — `./build-debug/tests/nexa_tests "Voice settings for the wake name*"` fails to compile.
- [ ] **Step 3: Implement** the seven fields with the defaults; unknown sensitivity/pause values and an empty name fall
  back to the defaults in `fromJson`.
- [ ] **Step 4: Run it** — passes; the whole suite passes.
- [ ] **Step 5: Commit** — `M16: voice settings for the wake name, listening and the cloud voice`.

### Task 2: WordPieces — the spotter's word pieces for any name

**Files:**
- Create: `voice/wake/WordPieces.h`, `voice/wake/WordPieces.cc`
- Modify: `voice/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/WordPiecesTest.cc`

**Interfaces:**
- Produces:
  ```cpp
  class WordPieces {   // Strategy-free value type; pure
  public:
      explicit WordPieces(QHash<QString, float> normalPieces);
      static Result<WordPieces> fromModel(const QByteArray &modelProto);   // a SentencePiece ModelProto
      static Result<WordPieces> fromModelFile(const QString &path);
      QStringList split(const QString &text) const;    // "hey nexa" → {"▁HE","Y","▁NE","X","A"}
  };
  ```

- [ ] **Step 1: Write the failing tests**
  - `TEST_CASE("WordPieces splits like sentencepiece")` with this fixture (copied from the real `bpe.model`; `Q` is the
    model's lowest score, which sets the unknown-character cost):
    ```cpp
    const QHash<QString, float> kPieces{
        {u"S"_s, -2.9737f}, {u"T"_s, -3.5683f}, {u"N"_s, -4.1642f}, {u"E"_s, -4.1970f}, {u"D"_s, -4.2265f},
        {u"Y"_s, -4.5183f}, {u"▁"_s, -4.6242f}, {u"O"_s, -4.6626f}, {u"C"_s, -4.6637f}, {u"R"_s, -4.7055f},
        {u"A"_s, -4.7523f}, {u"U"_s, -4.8031f}, {u"▁S"_s, -4.9738f}, {u"G"_s, -5.0245f}, {u"L"_s, -5.0477f},
        {u"I"_s, -5.1948f}, {u"▁SO"_s, -5.2595f}, {u"OR"_s, -5.2667f}, {u"AR"_s, -5.2759f}, {u"▁HE"_s, -5.3153f},
        {u"K"_s, -5.4402f}, {u"W"_s, -5.4856f}, {u"LL"_s, -5.5720f}, {u"▁T"_s, -5.6011f}, {u"▁W"_s, -5.7251f},
        {u"ON"_s, -5.8591f}, {u"RA"_s, -5.9295f}, {u"SE"_s, -6.0779f}, {u"V"_s, -6.0915f}, {u"NE"_s, -6.1160f},
        {u"H"_s, -6.1748f}, {u"NG"_s, -6.2455f}, {u"LO"_s, -6.2875f}, {u"CK"_s, -6.2890f}, {u"EL"_s, -6.3479f},
        {u"▁O"_s, -6.3548f}, {u"VI"_s, -6.6003f}, {u"Z"_s, -6.6597f}, {u"IS"_s, -6.7041f}, {u"US"_s, -6.7396f},
        {u"X"_s, -6.7431f}, {u"TA"_s, -7.0060f}, {u"HE"_s, -7.1057f}, {u"J"_s, -7.2631f}, {u"▁NE"_s, -7.2809f},
        {u"▁TA"_s, -7.5500f}, {u"▁WORLD"_s, -7.7007f}, {u"▁JA"_s, -7.9239f}, {u"▁NEXT"_s, -8.1131f},
        {u"▁OKAY"_s, -8.2925f}, {u"Q"_s, -9.8577f}};
    ```
    and `split(text).join(u' ')` equal to: `hey nexa → ▁HE Y ▁NE X A`, `hey jarvis → ▁HE Y ▁JA R VI S`,
    `hey tauseeq → ▁HE Y ▁TA U SE E Q`, `hello world → ▁HE LL O ▁WORLD`, `necksa → ▁NE CK S A`,
    `okay nexa → ▁OKAY ▁NE X A`, `hey zara → ▁HE Y ▁ Z AR A`, `next song → ▁NEXT ▁SO NG`.
  - `TEST_CASE("WordPieces reads a SentencePiece model and refuses a BPE one")`: a test helper
    `QByteArray modelProto(const QHash<QString, float> &pieces, int modelType)` writes field 1 (`piece` 1 string,
    `score` 2 fixed32 float, `type` 3 varint = 1) per piece and field 2 (TrainerSpec, `model_type` 3 varint);
    `fromModel(modelProto(kPieces, 1))` splits "hey nexa" like the table; `modelType 2` → `ErrorCode::Parse` with
    "not a SentencePiece unigram model"; a missing file → `ErrorCode::Io`.
- [ ] **Step 2: Run them** — fail to compile.
- [ ] **Step 3: Implement.** Text: upper-case, spaces → `▁`, a `▁` prepended. Viterbi over the NORMAL pieces (type 1),
  best total score; a single character that is no piece costs (lowest score − 10). Protobuf: varint / length-delimited /
  fixed32 / fixed64 fields, unknown fields skipped; no TrainerSpec counts as unigram (sentencepiece's default).
- [ ] **Step 4: Run them** — pass.
- [ ] **Step 5: Commit** — `M16: WordPieces splits a name into the spotter's word pieces`.

### Task 3: WakePhrases — spellings, keyword lines, tuning and the name check

**Files:**
- Create: `voice/wake/WakePhrases.h`, `voice/wake/WakePhrases.cc`
- Modify: `voice/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/WakePhrasesTest.cc`

**Interfaces:**
- Consumes: `WordPieces::split` (Task 2).
- Produces:
  ```cpp
  enum class WakeSensitivity { Low, Normal, High };
  struct SpotterTuning { float boost{2.0f}; float threshold{0.10f}; float sileroThreshold{0.5f}; double startMarginDb{13.0}; };
  class WakePhrases {   // pure
  public:
      static Result<void> check(const QString &name);
      static QStringList spellings(const QString &name);                  // lower case, the name first
      static Result<QString> keywordLines(const QString &name, const WordPieces &pieces);
      static bool isGreeted(const QString &label);                        // "HEY_NEXA" → true, "NEXA" → false
      static WakeSensitivity sensitivityFrom(const QString &value);       // "low" | "normal" | "high"
      static SpotterTuning tuning(WakeSensitivity sensitivity);
  };
  ```

- [ ] **Step 1: Write the failing tests**
  - `spellings(u"Nexa") == {"nexa","necksa","neksa"}`; `spellings(u"Jarvis") == {"jarvis"}`;
    `spellings(u"Phoenix") == {"phoenix","phoenicks","phoeniks","foenix"}` (rules in order `x→cks`, `x→ks`, `ph→f`,
    `q→k`, `c` before a/o/u `→k`; at most three extra); `spellings(u"Mister-Robot") == {"mister robot"}`.
  - `keywordLines(u"Nexa", pieces)` (pieces = Task 2's table) has 15 lines, contains `▁HE Y ▁NE X A @HEY_NEXA`,
    `▁OKAY ▁NE X A @OKAY_NEXA`, `▁NE X A @NEXA`, `▁NE CK S A @NECKSA`; no label contains a space.
  - `isGreeted(u"OK_NEXA")`, `!isGreeted(u"NECKSA")`.
  - `tuning(Low/Normal/High)` equals the Global Constraints values; `sensitivityFrom(u"loud") == Normal`.
  - `check(u"Jarvis")` ok; `check(u"N3")` → `InvalidArgument` "Use at least two letters."; `check(u"نیکسا")` →
    "Use Latin letters (a–z) so the name can be spotted instantly."; a 25-character name → "Use 2–24 characters."
- [ ] **Step 2: Run them** — fail to compile.
- [ ] **Step 3: Implement** to the tests: forms per spelling `hey s`, `ok s`, `okay s`, `hi s`, `s`; label = upper case
  with spaces → `_`; `keywordLines` returns `check`'s error first.
- [ ] **Step 4: Run them** — pass.
- [ ] **Step 5: Commit** — `M16: WakePhrases builds the spotter's keywords from any name`.

### Task 4: WakeWordDetector — any name, anywhere

**Files:**
- Modify: `voice/pipeline/WakeWordDetector.h/.cc`, `voice/pipeline/VoicePipeline.h/.cc` (an instance from
  `Settings::wakeName`), `tests/VoicePureTest.cc` (replace the static test), `tests/VoicePipelineTest.cc` (compiles)

**Interfaces:**
- Consumes: `WakePhrases::spellings` (Task 3).
- Produces:
  ```cpp
  class WakeWordDetector {   // pure
  public:
      explicit WakeWordDetector(const QString &name = QStringLiteral("Nexa"));
      struct Match { QString command; int firstWord{0}; bool greeted{false}; };   // firstWord: the greeting's, or the name's
      std::optional<Match> find(const QString &transcript) const;   // anywhere
      QString commandOf(const QString &transcript) const;           // greeting + name off the start; else unchanged
      QString recognizerPrompt() const;                              // "Hey ‹Name›."
      QString name() const;
      static QString soundKey(const QString &word);
  };
  // VoicePipeline::Settings gains: QString wakeName{QStringLiteral("Nexa")};
  ```

- [ ] **Step 1: Write the failing test** `TEST_CASE("WakeWordDetector finds any name anywhere")` (words = whitespace tokens):
  ```cpp
  const WakeWordDetector nexa(u"Nexa"_s);
  const auto mid = nexa.find(u"I was just saying that, hey Nexa, pause the music."_s);
  REQUIRE(mid); CHECK(mid->command == u"pause the music."_s); CHECK(mid->firstWord == 5); CHECK(mid->greeted);
  CHECK(nexa.find(u"Hey Nexus, what time is it?"_s)->command == u"what time is it?"_s);
  CHECK(nexa.find(u"heynecks, pause it"_s)->command == u"pause it"_s);
  CHECK(nexa.find(u"OK Neksa. Play some music."_s)->command == u"Play some music."_s);
  CHECK(nexa.find(u"That's it. Nexa, stop."_s)->command == u"stop."_s);
  CHECK(nexa.find(u"Nexa open files"_s)->command == u"open files"_s);
  CHECK(nexa.find(u"  Hey,  Nexa!"_s)->command.isEmpty());
  for (const QString &text : {u"Next song please"_s, u"The Nexus phone is really good"_s, u"I need an extra hour"_s,
                              u"I told Nexa nothing"_s, u"Hey Nick, can you pass the salt?"_s, u"Hey, can you hear me?"_s})
      CHECK_FALSE(nexa.find(text));
  const WakeWordDetector jarvis(u"Jarvis"_s);
  CHECK(jarvis.find(u"Hey Jervis, open YouTube"_s)->command == u"open YouTube"_s);
  CHECK_FALSE(jarvis.find(u"I love Jarvis movies"_s));
  CHECK(jarvis.commandOf(u"Hey Jarvis."_s).isEmpty());
  CHECK(jarvis.commandOf(u"open YouTube"_s) == u"open YouTube"_s);
  CHECK(WakeWordDetector(u"Mister Robot"_s).find(u"hey mister robot, lights off"_s)->command == u"lights off"_s);
  CHECK(WakeWordDetector::soundKey(u"Nexa"_s) == u"nks"_s);
  CHECK(WakeWordDetector::soundKey(u"Necksa"_s) == u"nks"_s);
  CHECK(WakeWordDetector::soundKey(u"Jarvis"_s) == u"jrvs"_s);
  CHECK(nexa.recognizerPrompt() == u"Hey Nexa."_s);
  ```
- [ ] **Step 2: Run it** — fails to compile.
- [ ] **Step 3: Implement.** Greeted: a token whose letters are `hey|hi|hello|ok|okay|yo` (or start with one — "heynecks")
  followed by 1…(name words + 1) tokens whose joined sound key is within one edit of the name's, the candidate key
  **not shorter** than the name's (so "Nick" ≠ "Nexa"); names with a key under three letters match exactly. Bare: a
  whole token equal to a spelling at the transcript's start or after a token ending in `. , ! ? ; :`. Sound key:
  letters only, lower case; `ck→k`, `c→k`, `q→k`, `x→ks`, `ph→f`, `z→s`; doubled letters single; vowels and `y`
  dropped after the first letter. The pipeline swaps `WakeWordDetector::match(...)` for its instance's `find(...)` and
  `commandOf(...)`; `recognizerPrompt()` comes from the instance.
- [ ] **Step 4: Run it** — passes; the whole suite passes (the pipeline's wake test still finds "Hey Nexa, …").
- [ ] **Step 5: Commit** — `M16: the wake name is anyone's, found anywhere in a sentence`.

### Task 5: The fast path takes the chosen name off

**Files:**
- Modify: `core/intent/IntentRouter.h/.cc`
- Test: `tests/IntentRouterTest.cc`

**Interfaces:**
- Produces: `void IntentRouter::setAssistantName(const QString &name);` and
  `static IntentInput IntentRouter::prepare(const QString &text, const QString &assistantName = QStringLiteral("Nexa"));`
  (`route()` passes its name).

- [ ] **Step 1: Write the failing test** `TEST_CASE("The fast path takes off the name the user chose")`:
  `IntentRouter::prepare(u"hey jarvis, open firefox"_s, u"Jarvis"_s).text == u"open firefox"_s`; with `Fixture f;
  f.router.setAssistantName(u"Jarvis"_s);` → `toolOf(f.router, u"hey jarvis, volume 50"_s) == u"set_volume"_s`;
  `IntentRouter::prepare(u"Hey Nexa, could you please open Firefox for me!"_s).text` is unchanged from today.
- [ ] **Step 2: Run it** — fails.
- [ ] **Step 3: Implement** the leading filler per name (the name regex-escaped, `(?:hey|hi|ok|okay|yo)\s+‹name›|‹name›`
  plus today's politeness words).
- [ ] **Step 4: Run it** — passes.
- [ ] **Step 5: Commit** — `M16: the fast path understands the chosen name`.

### Task 6: Word times from the offline recogniser

**Files:**
- Modify: `voice/stt/ISpeechRecognizer.h`, `voice/stt/WhisperRecognizer.h/.cc`
- Test: `tests/VoicePureTest.cc`

**Interfaces:**
- Produces:
  ```cpp
  struct TimedWord { QString text; int startMs{0}; int endMs{0}; };
  struct TimedTranscript { QString text; QList<TimedWord> words; };
  // ISpeechRecognizer:
  using DoneWords = std::function<void(Result<TimedTranscript>)>;
  virtual void transcribeWords(const QByteArray &pcm16k, const QString &prompt, DoneWords done);  // default: transcribe(), no words
  ```

- [ ] **Step 1: Write the failing test** `TEST_CASE("A recogniser without word times still answers transcribeWords")`: a
  minimal recogniser answering "hello there" → `TimedTranscript{text "hello there", words empty}`; an error passes
  through unchanged.
- [ ] **Step 2: Run it** — fails to compile.
- [ ] **Step 3: Implement** the default in the interface; `WhisperRecognizer::transcribeWords` runs the same job with
  `token_timestamps = true`, `max_len = 1`, `split_on_word = true`, `no_timestamps = false`, one `TimedWord` per segment
  (t0/t1 are centiseconds), the text the segments joined and trimmed. (Checked end to end in Task 18.)
- [ ] **Step 4: Run it** — passes.
- [ ] **Step 5: Commit** — `M16: whisper says when each word was spoken`.

### Task 7: The listening front — the pipeline hears through it (behaviour unchanged)

**Files:**
- Create: `voice/listen/AudioRing.h/.cc`, `voice/listen/IVoiceDetector.h`, `voice/listen/EnergyVoiceDetector.h/.cc`,
  `voice/listen/IAudioFilter.h`, `voice/wake/IWakeSpotter.h`, `voice/listen/ListeningFront.h/.cc`
- Modify: `voice/capture/EnergyVad.h/.cc` (spans), `voice/pipeline/VoicePipeline.h/.cc`, `voice/CMakeLists.txt`,
  `tests/CMakeLists.txt`
- Test: `tests/ListeningFrontTest.cc`, `tests/VoicePureTest.cc`, `tests/VoicePipelineTest.cc`

**Interfaces:**
- Produces:
  ```cpp
  // EnergyVad: struct Span { qint64 start{0}; qint64 end{0}; };  Span lastSpan() const;  // samples since reset()
  class AudioRing {   // pure: the last seconds of raw audio, by sample position since reset()
  public:
      explicit AudioRing(int seconds = 10);   // 10, not the spec's 4: second-net utterances (≤ 8 s) are cut from it too
      void append(QByteArrayView pcm16k);  void reset();
      qint64 begin() const;  qint64 end() const;
      QByteArray slice(qint64 from, qint64 to) const;   // clamped to [begin, end)
      double levelDb(qint64 from, qint64 to) const;      // RMS, dBFS
  };
  class IVoiceDetector {   // Strategy: where speech starts and ends (positions in samples since reset())
  public:
      struct Event { enum class Kind { Started, Ended } kind; qint64 start{0}; qint64 end{0}; };
      virtual ~IVoiceDetector() = default;
      virtual QString name() const = 0;
      virtual QList<Event> feed(QByteArrayView pcm16k) = 0;
      virtual void reset() = 0;
      virtual void setEndPauseMs(int ms) = 0;
      virtual void setMaxUtteranceMs(int ms) = 0;
  };
  class EnergyVoiceDetector : public IVoiceDetector { public: explicit EnergyVoiceDetector(double startMarginDb = 14.0); … };
  class IAudioFilter { public: virtual ~IAudioFilter() = default; virtual QByteArray process(QByteArrayView pcm16k) = 0; virtual void reset() = 0; };
  class IWakeSpotter { public:
      struct Hit { QString label; qint64 phraseStart{0}; qint64 heardAt{0}; };   // samples since reset()
      virtual ~IWakeSpotter() = default; virtual QList<Hit> feed(QByteArrayView pcm16k) = 0; virtual void reset() = 0; };
  class ListeningFront : public QObject {   // Facade: filter → detector + spotter; inline or on a worker thread
      Q_OBJECT
  public:
      enum class Threading { Inline, Worker };
      struct Parts { std::unique_ptr<IVoiceDetector> detector; std::unique_ptr<IWakeSpotter> spotter; std::unique_ptr<IAudioFilter> filter; };
      ListeningFront(Parts parts, Threading threading, QObject *parent = nullptr);
      QString detectorName() const;  bool hasSpotter() const;
      void feed(const QByteArray &pcm16k);   // raw; positions count from the last reset()
      void reset();                           // events still queued from before are dropped
      void setEndPauseMs(int ms);  void setMaxUtteranceMs(int ms);
  signals:
      void speechStarted(qint64 start);
      void speechEnded(qint64 start, qint64 end);
      void wakeHeard(const QString &label, qint64 phraseStart, qint64 heardAt);
  };
  // VoicePipeline: void setListeningFront(std::unique_ptr<ListeningFront> front);
  // (the constructor installs an inline front with an EnergyVoiceDetector, so behaviour is today's)
  ```

- [ ] **Step 1: Write the failing tests**
  - `TEST_CASE("EnergyVad reports where the utterance was")`: quiet(500)+speech(600)+quiet(1200) → `lastSpan().start`
    within 320 samples of 3200 (pre-roll 300 ms), `end` within 320 of 20800 (trailing 200 ms kept).
  - `TEST_CASE("AudioRing keeps the last seconds by position")`: 3 s ring, append 5 s → `begin() == 32000`,
    `end() == 80000`; `slice(0, 40000).size() == (40000 - 32000) * 2`; `levelDb` of a `testaudio::speech` stretch is
    within 1 dB of −15.
  - `TEST_CASE("The inline front reports speech and the spotter's hits by position")`: an `EnergyVoiceDetector` and a
    fake spotter returning one hit at sample 8000 → `speechStarted`, `speechEnded` (span as in the EnergyVad test),
    `wakeHeard(u"HEY_NEXA"_s, 8000, …)`; after `reset()` positions start at 0 again.
  - `TEST_CASE("The worker front reports the same events on the GUI thread")`: the same parts, `Threading::Worker`,
    `QSignalSpy::wait` → same positions; signals arrive on the test's thread.
  - `TEST_CASE("The filter feeds the detector and the spotter; the ring keeps the raw audio")`: an `IAudioFilter` that
    halves every sample → the fake detector and spotter receive halved audio, `AudioRing::slice` returns the original.
  - `TEST_CASE("A failing microphone leaves the pipeline idle and the front reset")` (Review Focus 3): wake word on, the
    fake microphone emits `failed` mid-utterance → notice emitted, `state() == Idle`, a following hit from the old
    utterance is not acted on.
- [ ] **Step 2: Run them** — fail to compile.
- [ ] **Step 3: Implement.** The pipeline: `onAudio` appends to the ring, feeds the front, and records for push-to-talk
  as today; `speechEnded(start, end)` takes the utterance from the ring (raw audio) where it used `takeUtterance()`;
  `updateMicrophone` resets ring and front when the microphone opens. Worker threading: one `QThread` per front, the
  parts moved with it, events back through queued signals with a generation number checked against `reset()`.
- [ ] **Step 4: Run them** — the new tests and **every existing `VoicePipelineTest` case** pass unchanged.
- [ ] **Step 5: Commit** — `M16: the pipeline hears through a listening front (no change in behaviour)`.

### Task 8: The pause before "I'm done"

**Files:**
- Modify: `voice/pipeline/VoicePipeline.h/.cc`
- Test: `tests/VoicePipelineTest.cc`

**Interfaces:**
- Produces: `VoicePipeline::Settings` gains `int endPauseMs{1400};` (the front's pause while a request is being
  captured; 700 ms while only the name is listened for). Caps: push-to-talk 60 s (today's `kMaxListenMs`), after the
  name or in a follow-up 15 s (used by Tasks 9–11).

- [ ] **Step 1: Write the failing test** `TEST_CASE("A pause shorter than the setting does not end the request")`: tap
  mode, `endPauseMs 1400`: speech(600)+quiet(1200)+speech(600)+quiet(1500) → one recognition whose audio holds both
  speech stretches; with `endPauseMs 900` the same input ends after the first stretch.
- [ ] **Step 2: Run it** — fails (today's 900 ms ends it at the first pause).
- [ ] **Step 3: Implement**: `setSettings` pushes the pause to the front; `startListening` and request capture use
  `endPauseMs`; the wake segmentation uses 700 ms and an 8 s longest utterance.
- [ ] **Step 4: Run it** — passes; whole suite passes.
- [ ] **Step 5: Commit** — `M16: a pause of the owner's choosing ends a request`.

### Task 9: The second net — the name anywhere, the request through the request recogniser

**Files:**
- Modify: `voice/pipeline/VoicePipeline.h/.cc`
- Test: `tests/VoicePipelineTest.cc` (the fake recogniser gains `transcribeWords` answering given words)

**Interfaces:**
- Consumes: `WakeWordDetector::find/commandOf` (Task 4), `ISpeechRecognizer::transcribeWords` (Task 6), `AudioRing`
  (Task 7).
- Produces: `static QString VoicePipeline::requestPrompt(const QString &wakeName);` (the Global Constraints text).

- [ ] **Step 1: Write the failing tests**
  - `"Second net: the name mid-utterance → the request from the word time"`: a 3 s utterance; the local recogniser
    answers words `I 0–300, was …, hey 2000–2200, Nexa, 2200–2600, pause 2600–2900, the …, music. …` → the request
    recogniser gets `(3000 - 1800) * 32` bytes (from 0.2 s before "hey") with `requestPrompt(u"Nexa"_s)`; it answers
    "Hey Nexa, pause the music." → request "pause the music.".
  - `"Second net: a failing request recogniser → the offline text after the name"`: the request recogniser answers an
    error → request "pause the music." (from the local transcript).
  - `"An utterance arriving while whisper is busy is checked afterwards, not dropped"`: two utterances, the second while
    the first is still with the local recogniser → both transcribed in turn; a third arriving while the first is busy
    replaces the second (newest wins).
  - `"The name alone: Nexa listens for the request"`: local "Hey Nexa." → `Listening`; the next utterance goes to the
    request recogniser → request; nothing within 8 s → `Idle`.
  - `"Only the name back from the recogniser: Nexa waits for the request"` (Review Focus 5): request recogniser answers
    "Hey Nexa." → `Listening`, no "I didn't catch that." notice.
  - `"A request started by the name ends at 15 s when the room never goes quiet"`: after the name, 16 s of speech →
    recognised at 15 s.
- [ ] **Step 2: Run them** — fail.
- [ ] **Step 3: Implement.** The words map to the transcript's whitespace tokens by index; when the counts differ, or
  there are no words, the offline text after the name is submitted (no cloud request). `transcribe()` (push-to-talk) and
  the request path use `requestPrompt(name)` and `commandOf`.
- [ ] **Step 4: Run them** — pass; whole suite passes.
- [ ] **Step 5: Commit** — `M16: the second net finds the name anywhere and sends only the request`.

### Task 10: The spotter wakes Nexa

**Files:**
- Modify: `voice/pipeline/VoicePipeline.h/.cc`
- Test: `tests/VoicePipelineTest.cc` (a fake spotter in the rig's inline front)

**Interfaces:**
- Consumes: `ListeningFront::wakeHeard` (Task 7), `WakePhrases::isGreeted` (Task 3).

- [ ] **Step 1: Write the failing tests**
  - `"A spotter hit mid-speech: Listening at once; the request from 0.3 s before the phrase"`: speech from 0 to 4 s, a
    greeted hit with `phraseStart` = 2.0 s → `state() == Listening` before the speech ends; the request recogniser gets
    audio from 1.7 s to the end; "Hey Nexa, open YouTube" back → request "open YouTube"; the utterance that held the
    hit is not sent to the second net.
  - `"A bare-name hit counts only when the name opens the utterance"`: speech starting at 0, a bare hit at 1.5 s →
    ignored; speech starting at 2.0 s with a bare hit at 2.3 s → Listening.
  - `"A hit while speaking stops the speech; Nexa's own name in its speech is ignored"`: Speaking "The weather is fine."
    + greeted hit → speech stopped, `interruptRequested` if a reply streams, Listening; Speaking "I am Nexa, your
    assistant." + hit within 3 s → ignored.
  - `"A hit from a replaced listening front is ignored"` (Review Focus 1): a hit queued by a worker front, then
    `setListeningFront(...)` with a new one → no Listening.
  - `"Talk while capturing a request after the name: one request, from the key"` (Review Focus 4): spotter hit →
    Listening; `startListening(true)`, speak, `stopListening()` → exactly one recognition, from the key's recording.
- [ ] **Step 2: Run them** — fail.
- [ ] **Step 3: Implement** to the tests and the Global Constraints timings (bare rule: the front's last
  `speechStarted` at most 500 ms before `phraseStart`; self-guard: a spoken sentence of the last 3 s in which
  `WakeWordDetector::find` or a spelling occurs).
- [ ] **Step 4: Run them** — pass; whole suite passes.
- [ ] **Step 5: Commit** — `M16: the spotter wakes Nexa the moment the name is said`.

### Task 11: Follow-ups without the name

**Files:**
- Modify: `voice/pipeline/VoicePipeline.h/.cc`
- Test: `tests/VoicePipelineTest.cc`

**Interfaces:**
- Produces: `VoicePipeline::Settings` gains `bool followUps{true};`.

- [ ] **Step 1: Write the failing tests**
  - `"A follow-up within 8 s of the spoken reply is a request without the name"`: a voice request, a spoken reply,
    `finishSpeaking()` → `Listening`; after 150 ms a new speech onset at the request's level → the request
    recogniser → request with source voice.
  - `"Silence after the reply ends the window"`: nothing for 8 s → `Idle`.
  - `"No follow-up after a typed request or with the setting off"`.
  - `"Speech already going on, or much quieter than the request, does not count"`: speech playing through the window's
    opening; a later onset 10 dB below the last request's level → `Idle` at 8 s.
  - `"Talk in the follow-up window: one request, from the key"` (Review Focus 4).
- [ ] **Step 2: Run them** — fail.
- [ ] **Step 3: Implement**: keep the RMS level (`AudioRing::levelDb`) of each request's speech; the window opens the
  microphone even with the wake word off (`updateMicrophone` counts it as listening).
- [ ] **Step 4: Run them** — pass; whole suite passes.
- [ ] **Step 5: Commit** — `M16: follow-ups without the name`.

### Task 12: sherpa-onnx — spotter, Silero and the noise reducer

**Files:**
- Create: `voice/wake/sherpa/c-api.h` (v2.0.1, unchanged), `voice/wake/sherpa/LICENSE` (Apache-2.0),
  `voice/wake/SherpaRuntime.h/.cc`, `voice/wake/KeywordSpotter.h/.cc`, `voice/listen/SileroVoiceDetector.h/.cc`,
  `voice/listen/NoiseReducer.h/.cc`, `scripts/download-sherpa.sh`
- Modify: `voice/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/SherpaTest.cc`

**Interfaces:**
- Consumes: `IWakeSpotter`, `IVoiceDetector`, `IAudioFilter` (Task 7), `SpotterTuning` (Task 3).
- Produces:
  ```cpp
  class SherpaRuntime {   // Facade: sherpa-onnx's C API loaded with QLibrary from <dir>/lib (nothing linked)
  public:
      explicit SherpaRuntime(QString dir = defaultDir());   // ~/.local/share/nexa/sherpa
      static QString defaultDir();
      bool load();  bool available() const;  QString problem() const;
      QString kwsDir() const;  QString sileroModel() const;  QString gtcrnModel() const;
      // one member per function, e.g. decltype(&SherpaOnnxCreateKeywordSpotter) createKeywordSpotter{};
  };
  class KeywordSpotter : public IWakeSpotter {
  public:
      static Result<std::unique_ptr<KeywordSpotter>> create(SherpaRuntime *runtime, const QString &keywordLines, SpotterTuning tuning);
      static qint64 phraseStartSample(float startTime, float firstPieceTime);   // seconds → samples (16 kHz)
  };
  class SileroVoiceDetector : public IVoiceDetector {
  public: static Result<std::unique_ptr<SileroVoiceDetector>> create(SherpaRuntime *runtime, float threshold); };
  class NoiseReducer : public IAudioFilter {
  public: static Result<std::unique_ptr<NoiseReducer>> create(SherpaRuntime *runtime); };
  ```
  Functions resolved: the keyword spotter's ten (`CreateKeywordSpotter`, `DestroyKeywordSpotter`, `CreateKeywordStream`,
  `DestroyOnlineStream`, `OnlineStreamAcceptWaveform`, `IsKeywordStreamReady`, `DecodeKeywordStream`,
  `GetKeywordResult`, `DestroyKeywordResult`, `ResetKeywordStream`), the VAD's (`CreateVoiceActivityDetector`,
  `DestroyVoiceActivityDetector`, `VoiceActivityDetectorAcceptWaveform`, `VoiceActivityDetectorEmpty`,
  `VoiceActivityDetectorDetected`, `VoiceActivityDetectorFront`, `VoiceActivityDetectorPop`,
  `DestroySpeechSegment`, `VoiceActivityDetectorReset`), the denoiser's (`CreateOnlineSpeechDenoiser`,
  `DestroyOnlineSpeechDenoiser`, `OnlineSpeechDenoiserRun`, `OnlineSpeechDenoiserReset`, `DestroyDenoisedAudio`).

- [ ] **Step 1: Write the failing tests**
  - `"Without the library, the runtime says how to install it"`: `SherpaRuntime(u"/nonexistent"_s).load()` false,
    `problem()` contains `scripts/download-sherpa.sh`.
  - `"The spotter places the phrase from its timestamps"`: `phraseStartSample(2.0f, 0.24f) == 35840`.
  - Installed only (tagged `[sherpa]`, `SKIP` otherwise, through `SherpaRuntime().load()`): the spotter built from `keywordLines(u"Nexa")`
    and the installed `bpe.model` hears nothing in 2 s of `testaudio::quiet`; Silero reports no speech for a 220 Hz
    tone (`testaudio::speech`) — a tone is not a voice; the noise reducer returns as many samples as it was given,
    within one frame.
- [ ] **Step 2: Run them** — fail to compile.
- [ ] **Step 3: Implement.** Spotter config as the probe (`feat` 16000/80, int8 encoder/decoder/joiner, tokens,
  1 thread, `max_active_paths` 4, `num_trailing_blanks` 1, `keywords_buf` = the lines, boost/threshold from the
  tuning); after a hit `ResetKeywordStream` (whether `start_time` keeps counting after a reset is checked in the
  installed test with two names in one recording; if it restarts, the spotter adds its own origin). Silero:
  `silero_vad.onnx`, threshold from the tuning, `min_silence_duration` = end pause, `min_speech_duration` 0.25,
  `window_size` 512, `max_speech_duration` = longest utterance; Started when `Detected()` turns 1, Ended per popped
  segment (start, start + n); a pause change recreates the VAD keeping its position. Noise reducer: GTCRN streaming,
  1 thread. `download-sherpa.sh`: the four pinned files (Global Constraints), each archive unpacked into a fresh
  temporary directory and only the listed files copied into the install layout; `--uninstall` removes
  `~/.local/share/nexa/sherpa`; no sudo; `set -euo pipefail`, quoted paths, the `fetch` helper of `download-piper.sh`.
- [ ] **Step 4: Run them** — pass (the installed tests skip until Task 18 installs the library).
- [ ] **Step 5: Commit** — `M16: sherpa-onnx's spotter, Silero and noise reduction, loaded at run time`.

### Task 13: The microphone check and "Lower it"

**Files:**
- Create: `voice/capture/MicrophoneCheck.h/.cc`, `app/MicrophoneLeveler.h/.cc`
- Modify: `system/audio/WpctlAudioController.h/.cc` (a target), `voice/pipeline/VoicePipeline.h/.cc`,
  `app/AppController.h/.cc` (`AppServices::microphoneVolume`), `app/main.cc` (a second `WpctlAudioController` with
  `Target::Microphone`), `app/MainWindow.h/.cc` (the status bar button; the wake hint), `app/CMakeLists.txt`,
  `voice/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/MicrophoneCheckTest.cc`

**Interfaces:**
- Produces:
  ```cpp
  class MicrophoneCheck {   // pure
  public:
      struct Stats { double clippedShare{0}; double floorDb{-100}; double p90Db{-100}; };
      void feed(QByteArrayView pcm16k, qint64 nowMs);
      bool overloaded() const;                        // > 5 % clipped (|x| ≥ 32700) over 3 s, or floor > −30 dBFS for 10 s
      std::optional<QString> notice(qint64 nowMs);    // at most once a minute while overloaded
      static Stats measure(QByteArrayView pcm16k);
      static std::optional<int> nextVolume(int percent, const Stats &measured);   // nullopt: stop here
  };
  // WpctlAudioController: enum class Target { Speaker, Microphone }; explicit WpctlAudioController(Target target = Target::Speaker, QObject *parent = nullptr);
  // VoicePipeline: void measureLevels(int ms, std::function<void(MicrophoneCheck::Stats)> done);  signal microphoneOverloaded(const QString &notice);
  // AppController: signal microphoneOverloaded(const QString &notice); slot void lowerMicrophone();
  class MicrophoneLeveler : public QObject { public: MicrophoneLeveler(WpctlAudioController *microphone, VoicePipeline *voice, QObject *parent = nullptr);
      void run(std::function<void(QString summary)> done); };
  ```
  Notice texts: `The microphone is overloaded (%1 % of the sound is clipped), so Nexa cannot tell when you speak. Lower
  the input volume in GNOME Settings → Sound → Input.` and, for the floor, `The microphone hears a loud noise all the
  time (%1 dBFS), so Nexa cannot tell when you speak. Lower the input volume in GNOME Settings → Sound → Input.`

- [ ] **Step 1: Write the failing tests**
  - `testaudio::speech` stretches with quiet between → not overloaded.
  - A full-scale square wave clipped on 52 % of samples for 3 s → overloaded; the notice contains `52 %`.
  - Noise at −20 dBFS for 10 s → overloaded (floor); for 9 s → not.
  - `notice(t)` once, `notice(t + 30000)` none, `notice(t + 61000)` again.
  - `nextVolume(100, {0.3, -10, -2}) == 95`; `nextVolume(20, {0.0, -45, -22}) == 15`;
    `nextVolume(20, {0.0, -45, -25}) == std::nullopt`; `nextVolume(5, {0.1, -20, -3}) == std::nullopt`.
- [ ] **Step 2: Run them** — fail to compile.
- [ ] **Step 3: Implement.** The pipeline feeds the check from `onAudio` and emits the notice; `measureLevels` keeps
  the microphone open for `ms` and answers `MicrophoneCheck::measure` of what it heard. The leveler: read the input
  volume, measure 2 s, set `nextVolume`, repeat; the summary says where it stopped ("Microphone input lowered from
  100 % to 15 %; nothing clips now." or "…stopped at 5 %; it still clips — move the laptop out of the fan's wind.").
  The status bar shows the notice with a **Lower it** button while it stands; the button calls `lowerMicrophone()`.
  While the microphone is overloaded, the core's hint in wake-word mode is the notice's first sentence instead of
  "Say “Hey …”".
- [ ] **Step 4: Run them** — pass; `cmake --build build-debug` clean.
- [ ] **Step 5: Commit** — `M16: Nexa says when the microphone is overloaded and can lower it`.

### Task 14: Choosing the cloud voice, and the front's factory

**Files:**
- Create: `voice/tts/CloudVoices.h/.cc`
- Modify: `voice/VoiceBackends.h/.cc`, `app/RemoteBridge.cc`, `app/AppController.cc` and `app/SettingsDialog.cc`
  (callers move to the `Config` overloads), `voice/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/VoiceBackendsTest.cc`

**Interfaces:**
- Consumes: Tasks 3, 7, 12.
- Produces:
  ```cpp
  struct VoiceProviderChoice { QString id; QString name; bool usable{false}; QString reason; };
  // VoiceBackends:
  static QList<VoiceProviderChoice> recognitionChoices(const Config &config);
  static QList<VoiceProviderChoice> speechChoices(const Config &config);
  static QList<const ProviderConfig *> recognitionProviders(const Config &config);   // the chain's cloud links, in order
  static const ProviderConfig *speechProvider(const Config &config);                 // null: none
  static Result<std::unique_ptr<ISpeechRecognizer>> createRecognizer(const Config &config, HttpClient *http);
  static Result<std::unique_ptr<ITextToSpeech>> createTextToSpeech(const Config &config, HttpClient *http);
  static std::vector<std::unique_ptr<ISpeechSynthesizer>> createSynthesizers(const Config &config, HttpClient *http, QStringList *problems = nullptr);
  struct FrontBuild { std::unique_ptr<ListeningFront> front; QStringList problems; };
  static FrontBuild createListeningFront(const VoiceConfig &voice, SherpaRuntime *sherpa, bool withSpotter);
  // CloudVoices (pure):
  static QStringList voicesFor(const QString &model);
  static bool isRecognitionModel(const QString &id);   // whisper | transcri | audio
  static bool isSpeechModel(const QString &id);        // tts | speech | orpheus | playai | audio
  ```

- [ ] **Step 1: Write the failing tests** (`Config` with providers opencode, agy, gemini (no key), groq (key,
  `sttModel` whisper-large-v3-turbo), anthropic-kind):
  - Automatic recognition: `recognitionProviders` == {groq}; with the first choice an OpenAI-compatible provider with
    its own `sttModel` → it first, then groq.
  - `sttProvider "groq"` → {groq}; `sttProvider "agy"` → {}.
  - `recognitionChoices` reasons, the kind deciding first: agy "takes text only", opencode "no speech models", the
    gemini- and anthropic-kind providers "Nexa's cloud voice speaks the OpenAI audio API" (with or without a key), an
    OpenAI-compatible provider without a key "no API key"; groq usable with an empty reason.
  - `speechProvider` with `ttsProvider ""` and no `ttsModel` anywhere → null; groq with `ttsModel` set → groq.
  - `CloudVoices::voicesFor(u"canopylabs/orpheus-v1-english"_s) == {autumn, diana, hannah, austin, daniel, troy}`;
    `voicesFor(u"tts-1"_s)` has the ten OpenAI voices; `isRecognitionModel(u"whisper-large-v3"_s)`,
    `!isRecognitionModel(u"qwen/qwen3.8-27b"_s)`, `isSpeechModel(u"canopylabs/orpheus-v1-english"_s)`.
  - `createListeningFront(voice, &missingRuntime, true)` → an `EnergyVoiceDetector` front, no spotter, a problem
    naming `scripts/download-sherpa.sh`.
- [ ] **Step 2: Run them** — fail.
- [ ] **Step 3: Implement**; callers (`AppController`, `RemoteBridge`, `SettingsDialog::testVoice`) move to the
  `Config` overloads, so the PC, the phone app and the web remote use one selection.
- [ ] **Step 4: Run them** — pass; whole suite passes.
- [ ] **Step 5: Commit** — `M16: one choice of cloud voice for the PC, the phone and the web remote`.

### Task 15: AppController — the front, the name and what the model is told

**Files:**
- Create: `core/agent/VoicePrompt.h/.cc`
- Modify: `app/AppController.h/.cc`, `app/main.cc` (a `SherpaRuntime`, `AppServices::sherpa`), `core/CMakeLists.txt`,
  `tests/CMakeLists.txt`
- Test: `tests/AgentTest.cc` (or a new `VoicePromptTest.cc` beside it)

**Interfaces:**
- Produces: `QString voicePromptParagraph(const QString &wakeName);` (pure), `AppServices::sherpa`
  (`SherpaRuntime *`), `void AppController::setWakeWordForced(bool on);` (this run only, never saved).

- [ ] **Step 1: Write the failing test** `TEST_CASE("Spoken messages: what the model is told")`:
  `voicePromptParagraph(u"Nexa"_s)` equals
  `Messages that start with [voice] were spoken, and your reply to them is read aloud: answer in one to three short sentences of plain text, without Markdown, lists or code. Their words come from speech recognition and may be wrong or spelled by how they sound, and the user may mix Urdu and English: read them by sound ("page in" may be the song "Pehchan"). When a spoken request is still unclear, ask one short question instead of guessing; the user can answer without saying your name. Reply in the language the user spoke, writing Urdu in Roman letters.`;
  `voicePromptParagraph(u"Jarvis"_s)` is the same plus `\n\nThe user calls you Jarvis; answer to that name.`
- [ ] **Step 2: Run it** — fails to compile.
- [ ] **Step 3: Implement**; `systemPrompt()` uses it. `applyVoiceConfig`: the recogniser and voice from Task 14's
  `Config` overloads; `createListeningFront(voice, sherpa, wakeWord || forced)` into the pipeline; pipeline settings
  `wakeName`, `endPauseMs` (900/1400/2000), `followUps`; `router->setAssistantName(voice.wakeName)`; the front's
  problems and an overloaded microphone into `statusChanged` / `microphoneOverloaded`.
- [ ] **Step 4: Run it** — passes; `cmake --build build-debug` clean.
- [ ] **Step 5: Commit** — `M16: Nexa listens with the front and answers to its name`.

### Task 16: Settings → Voice

**Files:**
- Modify: `app/SettingsDialog.h/.cc`, `app/SystemStatus.h` (`SherpaRuntime *sherpa`;
  `std::function<void()> lowerMicrophone`; `std::function<QString()> microphoneSummary`)

**Interfaces:**
- Consumes: Tasks 1, 3, 13, 14.

- [ ] **Step 1: Build the controls** (Global Constraints copy): wake checkbox; **Wake name** line edit with the hint
  "A distinctive name of two or three syllables is heard best."; **Sensitivity** (Low / Normal / High); **Pause before
  I'm done** (Short — 0.9 s / Normal — 1.4 s / Long — 2 s); **Keep listening for a follow-up after a spoken reply**;
  the noise checkbox; the spotter line; **Microphone:** the summary and **Lower it**. Recognition: the mode combo as
  today, **Cloud provider** (Automatic + `recognitionChoices`, unusable entries disabled as "agy — takes text only"),
  an editable **Model** combo filled from the chosen provider's `listModels` filtered by `isRecognitionModel`, and the
  "Using Groq — whisper-large-v3-turbo" line for Automatic. Speech output: the same, plus an editable **Voice** combo
  from `CloudVoices::voicesFor`. The chosen provider's `sttModel`/`ttsModel`/`ttsVoice` are written back to that
  provider. The Providers tab's three speech fields are removed.
- [ ] **Step 2: Refuse a bad name**: `accept()` runs `WakePhrases::check` and shows its message under the field
  instead of closing.
- [ ] **Step 3: Look at it** — a private copy as CLAUDE.md describes, its config copied from the owner's into
  `.superpowers/scratch/m16-settings/config/nexa/` (git-ignored; the keys stay on the machine), then
  `QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$PWD/.superpowers/scratch/m16-settings/config" ./build/app/nexa-assistant
  --new-instance --screenshot-settings .superpowers/scratch/m16-settings/shots`; the Voice tab shows every control,
  agy and opencode greyed with their reasons, Groq's models listed.
- [ ] **Step 4: Run the suite** — passes; `cmake --build build-debug` clean.
- [ ] **Step 5: Commit** — `M16: Settings → Voice — the name, listening, the microphone and the cloud voice`.

### Task 17: Texts, `--hear` and the docs

**Files:**
- Create: `voice/capture/RecordedAudioInput.h/.cc`
- Modify: `app/MainWindow.cc` (the hint), `android/PhoneWindow.cc:361`, `remote/web/app.js:173`, `app/main.cc`,
  `voice/CMakeLists.txt`, `README.md`, `CLAUDE.md`, `ARCHITECTURE.md`
- Test: `tests/VoicePureTest.cc`

**Interfaces:**
- Produces: `class RecordedAudioInput : public IAudioInput { public: void play(const QByteArray &pcm16k); signals: void drained(); };`
  (100 ms every 100 ms while started; silence after the queue; `drained()` 3 s after the last queued chunk); `main`
  option `--hear <file>` ("Play the WAV <file> into Nexa as if the microphone heard it, with the wake word on
  (repeatable; this run only).").

- [ ] **Step 1: Write the failing test** `"RecordedAudioInput plays in real time, then silence"`: 1 s queued →
  ten 3200-byte chunks over ≈1 s, then zero-valued chunks; `drained` after ≈4 s (`QSignalSpy::wait(6000)`).
- [ ] **Step 2: Run it** — fails to compile.
- [ ] **Step 3: Implement**; `--hear` swaps the microphone for a `RecordedAudioInput`, calls
  `setWakeWordForced(true)`, and queues each file as a step that finishes on `drained` + idle. The hint uses the
  configured name; the phone and web texts as in the Global Constraints. Docs: README's voice paragraph (the name,
  the spotter, `download-sherpa.sh`, follow-ups, Lower it); CLAUDE.md (runtime, layout of `voice/wake` and
  `voice/listen`, the privacy rule as spec §3.4, `--hear`); ARCHITECTURE.md (an M16 status row, deviations: the
  10 s ring, the bare-name rule by the voice detector).
- [ ] **Step 4: Run it** — passes; whole suite passes; `cmake --build build-debug` clean.
- [ ] **Step 5: Commit** — `M16: --hear, the name in the hints, and the docs`.

### Task 18: Install, prove it end to end, and check it live

- [ ] **Step 1: Install** — `scripts/download-sherpa.sh`; the four checksums pass; `./build-debug/tests/nexa_tests
  "[sherpa]"`-tagged installed tests now run and pass (no SKIP).
- [ ] **Step 2: Scripted runs** on a private copy (`--new-instance`, offscreen, the owner's providers linked, wake word
  forced by `--hear`) with clips from `.superpowers/scratch/kws` (copied to the private copy's folder, never committed):
  start / mid-sentence / bare / okay (clean), noise, TV −12 dB, fan 10 / 5 / 0 dB → the request reaches the chat
  through Groq; every `neg-*` clip → no request. Record the counts in ARCHITECTURE.md's deviations.
- [ ] **Step 3: The owner's settings** — stop Nexa, set `voice.language` to `auto`, start it again in its own scope
  (memory note *restart-nexa-after-fixes*).
- [ ] **Step 4: Live with the owner** — the fan on: **Lower it** if the check complains; "Hey Nexa" normally,
  mid-sentence, and with the TV on; a pause mid-request; "Pehchan" and "Ammi ko call karo"; a follow-up; interrupting by
  name; a new name in Settings working at once; Groq's voice after the owner accepts its terms (Test voice).
- [ ] **Step 5: Commit** — `M16: verified end to end` with the ARCHITECTURE.md results.
