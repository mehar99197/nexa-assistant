# M16 — voice: a wake name of the owner's choosing, heard instantly; requests heard right

Status: design, approved part by part in conversation on 2026-10-29; this document is for the owner's review.
After it: the Tailscale work (reach the phone and the PC from anywhere), already agreed.

## 1. What the owner asked for

- "Make voice controlling more intelligent", and "Hey Nexa" **given to me — I can change the name when I want**.
- Listening "sensitivity or capacity too slow — fix it". Asked which symptoms: **it misses "Hey Nexa"**, **it cuts me off
  when I pause**, **it mishears words** (names, Urdu). Not: reacting late.
- "More intelligent" means all of: **follow-ups** without the name; **Urdu + English**, mixed too; **the name anywhere**,
  even mid-sentence or with a TV or people talking; **smarter replies** (short, and a question back when unclear); and
  words recognised correctly.

Decisions taken in the conversation:
- **Groq after the name.** The room is still only ever checked offline for the name; once the name is heard, the
  request that follows it may go to a cloud recogniser — the owner's Groq `whisper-large-v3-turbo` (key already in
  Nexa). Offline whisper is the fallback.
- **Both detectors**: an offline keyword spotter (sherpa-onnx) wakes Nexa the moment the name is said; whisper keeps
  checking each sentence as a second net, now finding the name anywhere.
- Added the same day: **choose the cloud voice in Settings → Voice** from the providers that exist — recognition and
  speech output each with a provider, a model and (for output) a voice; and "set models from agy for voice", which
  cannot be done (§3.10).
- Found the same day: **"it's not listening with Hey Nexa"** — the microphone signal is overloaded (§2, §3.11).
- Assumptions put to the owner and not corrected: Urdu is wanted in **Roman letters** ("Ammi ko call karo") because
  the contacts and apps are named that way; **follow-ups** last about 8 s after Nexa finishes speaking and go to Groq
  like a request; Nexa **answers to the chosen name** in replies too.

Rules that stay: every action through `ToolExecutor::execute`; keys never logged or shown; nothing blocks the GUI
thread; `core` never includes QtWidgets; while the wake word is on, **room sound is heard only offline** — the change
is that the request after the name (and a follow-up) may now go to the cloud recogniser.

## 2. What was measured (2026-10-29, i5-8265U, throwaway probe in `.superpowers/scratch/kws`)

- Today's request path is not slow: whisper `base.en` transcribes 1.9 s of speech in 0.3 s and 5.7 s in 0.7 s.
  "…the **Pehchan** original soundtrack" came out as "…the **page in** original soundtrack" (English-only model).
- Today's wake check only accepts the name at the **start** of an utterance, drops utterances while whisper is busy,
  and ends an utterance after 0.9 s of quiet (`VoicePipeline::vadSettings`).
- Recognition used only the **first choice's** speech model; the first choice (opencode) has none, so Groq's
  `whisper-large-v3-turbo` was never used. Groq answered a test clip in **0.3 s** (2.2 s for the first, cold request).
- **sherpa-onnx v2.0.1** (released 2026-10-26; `linux-x64-shared-lib`, 9.7 MB, SHA-256
  `532c45bbd41036d83cd70606797529aa8086c2053395a58967736c727625b120`, matching GitHub's digest) with **`sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01`** (17.6 MB archive, int8 model
  ~5 MB, SHA-256 `f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a`; GitHub records no digest for it): C API `SherpaOnnxCreateKeywordSpotter` & co.; keywords as word pieces of the
  model's `bpe.model`. The library has `RPATH $ORIGIN` and loads its own `libonnxruntime.so` (1.28.2) beside it.
  - **Cost**: real-time factor 0.028 on one thread — under 3 % of one core while listening.
  - **Word pieces**: the model's `bpe.model` is a SentencePiece **unigram** model; a Viterbi split over its own pieces
    and scores reproduced sentencepiece exactly on 22 of 22 phrases (`hey nexa → ▁HE Y ▁NE X A`,
    `hey jarvis → ▁HE Y ▁JA R VI S`, `hey tauseeq → ▁HE Y ▁TA U SE E Q`, …). Nexa can split any name itself.
  - **Detection**, 202 synthetic clips (three Piper voices × three speeds; clean, fan-like noise at 10 dB SNR, a "TV" —
    another voice reading news — at −6 dB and −12 dB):

    | caught | spotter | whisper, today's check (start only) | whisper, name anywhere |
    |---|---|---|---|
    | "Nexa" clean / noise | 34/36 · 11/12 (with "Necksa" added) | 27/36 · 9/12 | 36/36 · 12/12 |
    | "Nexa" TV −6 / −12 dB | 2/12 · 2/12 | 0/12 · 1/12 | 3/12 · 4/12 |
    | "Jarvis" clean / noise | 36/36 · 12/12 | — | 36/36 · 12/12 |
    | "Jarvis" TV −6 / −12 dB | 7/12 · 8/12 | — | 9/12 · 7/12 |

    **0 false wakes** on 154 clips without the name ("Next song please", "The Nexus phone is really good", "Alexa,
    play some music", "Hey Nick…", 51 s of news), at every setting tried. An invented name is harder for both
    detectors; a distinctive two- or three-syllable name is heard far better.
  - **Timing**: the spotter fires 0.1–0.8 s after the name ends, often while the request is already being said. The
    result's per-piece timestamps place the **start** of the phrase well (first piece "HEY" at 2.24 s, whisper's word
    "hey" at 2.23 s) but its end only loosely (last piece at 2.80 s, whisper's "Nexa," ending at 3.33 s, detection at
    3.40 s). Whisper's word timestamps (`token_timestamps`, `max_len = 1`, `split_on_word`) are good in mid-utterance
    and loose at the very edges of a clip.

- **Groq's voice**: `/models` lists `canopylabs/orpheus-v1-english` and `canopylabs/orpheus-arabic-saudi` for speech
  output, `whisper-large-v3` and `whisper-large-v3-turbo` for recognition. Speech output answers 400
  `model_terms_required` until the account's owner accepts the model's terms once in Groq's console. The owner's
  other providers: opencode and agy have no speech models; Google Gemini has no key.
- **agy and audio**: no option takes a file or audio (`agy --help`), the stream input accepts text blocks only (M15),
  and nothing comes back but text — agy can neither hear a recording nor speak.
- **The microphone (why "Hey Nexa" stopped working)**: the wake check transcribed 173 utterances on Oct 24 and almost
  none since Oct 27; Talk requests now run to the 8 s "nothing said" limit (8064, 8128, 8042 ms) — the energy VAD never
  sees speech start or end. Measured from the default source at the owner's 100 % input: **52–76 % of the samples
  clipped**, median level −1.6 dBFS, the quietest 20 ms −24.6 dBFS (a quiet room is around −60 to −45). No voice code
  changed (M15 touched comments only) and no Nexa action touched the input volume (audit log). Easy Effects routes
  every recording through `easyeffects_source`, but its input chain is a pass-through (spectrum and level meters).
  Lower input volumes did not bring it down reliably (25 %: −24 and −10 dBFS in two tries; 30–40 %: median −2.7 and
  −0.8), so the room was loud at the time or the volume does not reach the mic's gain — settled with the owner by a
  controlled test (quiet, then "Hey Nexa").

## 3. Design

### 3.1 Flow

```
microphone ──► VoicePipeline ──► ring buffer (last 4 s + per-frame loudness)
                  │  ├─► KeywordSpotter (worker thread) ──heard(name, where)──┐
                  │  └─► EnergyVad ──utterance──► second net: whisper + words │
                  │                       └──name found (where)───────────────┤
                  │                                                           ▼
                  │                  request capture: from just before the name until the pause
                  │                                                           ▼
                  │               request recogniser: Groq (first provider with a speech model) → offline whisper
                  │                                                           ▼
                  └──────────────── request("…", source voice) ──► AppController ──► reply, spoken
                                                                              ▼
                                                     follow-up window (8 s): speech here is the next request
```

### 3.2 New pieces (`voice/wake/`)

- **`WordPieces`** (pure): reads a SentencePiece model file (a minimal protobuf reader: `pieces` = field 1 with
  `piece` 1, `score` 2, `type` 3) and splits text into its pieces by Viterbi over the NORMAL pieces' scores, a single
  unknown character costing (lowest score − 10) — exactly what sentencepiece does for a unigram model on upper-case
  ASCII. Text is upper-cased, spaces become `▁`, a `▁` is prepended. Returns an error for a file that is not a
  unigram model.
- **`WakePhrases`** (pure): from the wake name builds what both detectors look for.
  - The name is cleaned for the spotter to letters, spaces, apostrophes and hyphens (hyphens → spaces); 1–3 words.
  - **Spellings**: the name, plus sound-alikes from a short rule list, at most three extra: `x → cks`, `x → ks`,
    `ph → f`, `q → k`, `c` before a/o/u `→ k` ("Nexa" → "Necksa", "Neksa").
  - **Keyword lines** for the spotter, for every spelling: `hey ‹s›`, `ok ‹s›`, `okay ‹s›`, `hi ‹s›` and the bare `‹s›`,
    each as `‹pieces› @‹LABEL›` (the label without spaces: `@HEY_NEXA`; the probe found that a space there breaks the
    whole file). A label tells whether the hit was greeted.
  - **Sensitivity** → spotter settings: Low = boost 1.0, threshold 0.25; Normal = 2.0, 0.10; High = 3.0, 0.05 — and
    the VAD's start margin: Low 16 dB, Normal 13 dB, High 10 dB (today 14).
- **`IWakeSpotter`** (Strategy) and **`KeywordSpotter`**: sherpa-onnx's keyword spotter, loaded at run time with
  `QLibrary` from `~/.local/share/nexa/sherpa/lib/libsherpa-onnx-c-api.so`, against a vendored copy of v2.0.1's
  `c-api.h` (Apache-2.0, `voice/wake/sherpa/c-api.h`, nothing to link). The ten functions are resolved by name; a
  missing one means "not usable" with the reason. The spotter and its stream live on a worker thread; audio arrives
  in the pipeline's 100 ms chunks through queued calls. A hit is reported as `heard(label, phraseStartSample,
  heardAtSample)`, counted in samples since the spotter started — the start from the result's `start_time` and its
  first piece's timestamp; the stream is then reset. Changing the name or the sensitivity
  recreates the spotter (about 0.1 s, off the GUI thread). `KeywordSpotter::available()` says whether the library and
  the model are installed.
- **`RecordedAudioInput`** (`voice/capture/`): an `IAudioInput` that plays a WAV in real time, 100 ms at a time — for
  `--hear`.

### 3.3 Changed pieces

- **`WakeWordDetector`** becomes an object built from the wake name (`WakeWordDetector(const QString &name)`), used by
  the second net and to take the name off a transcript. In a transcript (lower-cased, punctuation kept for clause
  boundaries):
  - **Greeted** (`hey|hi|hello|ok|okay|yo`, also glued on: "heynecks") followed by one or two words whose **sound key**
    is within one edit of the name's — **anywhere** in the transcript. The sound key: letters only; `ck→k`, `c→k`,
    `q→k`, `x→ks`, `ph→f`, `z→s`; doubled letters single; vowels and `y` dropped after the first letter. Names whose
    key is shorter than three letters must match it exactly.
  - **Bare**: a spelling of the name, as a whole word, that **starts the transcript or follows clause punctuation**
    (`. , ! ? ; :`) — "…that. Nexa, pause it" wakes; "I love Jarvis movies" and "the Nexus phone" do not.
  - Returns the command after the name and the index of the phrase's first word (the greeting, or the name) for the
    cut.
  - `recognizerPrompt()` becomes `"Hey ‹Name›."` for the wake path; requests use the prompt of §3.5.
- **`VoicePipeline`**:
  - Keeps a **ring buffer** of the last 4 s of audio with a loudness flag per 20 ms frame.
  - **Spotter hit** (state Idle, Thinking or Speaking): a bare-name hit counts only when the name starts after a pause
    (≥ 250 ms quiet before it); greeted hits count anywhere. Then: the speech stops, a reply still streaming is
    interrupted (`interruptRequested`), the state becomes **Listening** at once, and the **request** is captured
    from **0.3 s before the phrase's start** (taken from the ring buffer) to the end of speech. The name is in that
    audio on purpose — its start is where the spotter is precise — and is taken off the transcript (§3.5); nothing
    said before it is in the request.
  - **Second net**: each VAD utterance in which the spotter did not fire is transcribed offline with word times
    (wake VAD: hangover 0.7 s, longest utterance 8 s). An utterance arriving while whisper is busy is **kept**: the
    newest one waits and runs next (older waiting ones are dropped as stale). A name found → the request audio is
    the utterance from 0.2 s before the phrase's first word (word times), sent to the request recogniser; if that
    fails, the offline text after the name is used.
  - **The name alone** (nothing after it within the utterance): Listening for up to 8 s for the request to start.
  - **End of the request**: the pause set in Settings — Short 0.9 s, **Normal 1.4 s**, Long 2.0 s — after speech;
    at most **15 s** for a request started by the name or a follow-up (a TV keeps the energy VAD in speech, so the
    cap ends it), 60 s for push-to-talk as today. Push-to-talk's tap mode uses the same pause.
  - **Self-guard**: while Nexa speaks, a hit (either detector) is ignored if a sentence spoken in the last 3 s contains
    the name — Nexa hearing itself say its name.
  - **Follow-up window** (when on): after the spoken reply to a spoken request has finished, the pipeline stays in
    Listening for 8 s (from 150 ms after the speech ended). A **new onset** of speech in it (the VAD's SpeechStarted —
    speech already going on when the window opens does not count) whose level is **within 8 dB of the owner's last
    request** (a TV across the room is quieter than the owner at the laptop) is recorded until the pause and goes to
    the request recogniser as the next request with source `voice`; silence → Idle. Not after a typed request, and
    not when nothing was spoken aloud. The level of each request's speech frames is kept for this.
- **`ISpeechRecognizer`** gains `transcribeWords(pcm, prompt, DoneWords)` → text plus words with start/end in ms.
  The default implementation returns the text without words; `WhisperRecognizer` fills them in (`token_timestamps`,
  `max_len = 1`, `split_on_word`, `no_timestamps = false`). The wake recogniser stays local-only.
- **`VoiceBackends::createRecognizer`**: the cloud links are the first choice's speech model **and then every other
  provider with a speech recognition model** (OpenAI-compatible, key present), in the list's order; offline whisper
  last. The owner's chain becomes Groq → whisper `base.en`.

### 3.4 Privacy (CLAUDE.md's rule, reworded)

The microphone opens for a press of Talk, for the 8 s follow-up window after a spoken reply (when that setting is
on), and continuously while the wake word is on — and then **only offline recognition hears the room** (the spotter
and whisper). Audio goes to a cloud recogniser only when it is addressed to Nexa: the request said with the name (cut
from just before the name — never the speech before it), a follow-up in the window, and push-to-talk as before.
Utterances without the name are still dropped unlogged.

### 3.5 Recognition of requests

- Language: what Settings → Voice says; the owner's becomes **auto** (Urdu and English). The local `base.en` ignores it.
- Prompt (the recogniser's spelling hint): `"Hey ‹Name›. Ammi ko call karo. Volume kam karo. YouTube par Pehchan OST
  chalao."` — Roman-Urdu examples to pull Urdu toward Latin letters, names toward their spelling.
- The greeting and the name are taken off the start of the transcript (`WakeWordDetector`); the cut keeps them on
  purpose (§3.3). A transcript that starts without them is used as it is.
- If Groq writes Urdu in Urdu script anyway, it still goes to the model as said; verified live with the owner (§6).

### 3.6 What the model is told (`AppController::systemPrompt`)

The `[voice]` paragraph becomes: spoken messages are read aloud — answer in one to three short sentences of plain
text; the words come from speech recognition and may be wrong or spelled by sound, and the user may mix Urdu and
English — read them by how they sound ("page in" may be the song "Pehchan"); when a spoken request is still unclear,
ask one short question instead of guessing (the user can answer without saying the name); reply in the language the
user spoke, Urdu in Roman letters. And, whatever the source: "The user calls you ‹Name›." when the wake name is not
"Nexa".

### 3.7 Settings and config

`config.voice` gains (missing keys take the defaults; no version change):

| key | default | Settings → Voice |
|---|---|---|
| `wakeName` | `"Nexa"` | **Wake name** (2–24 characters, at least two letters) |
| `wakeSensitivity` | `"normal"` | **Sensitivity**: Low / Normal / High |
| `endPause` | `"normal"` | **Pause before I'm done**: Short / Normal / Long |
| `followUps` | `true` | **Keep listening for a follow-up after a spoken reply** |

The wake checkbox reads "Listen for my wake name — the microphone stays on; only offline recognition hears the room",
and an **Instant spotting** line says "Installed" or "Not installed — run `scripts/download-sherpa.sh`". Every change
applies when Settings is closed with OK, without a restart (`applyVoiceConfig`).

### 3.8 Texts

- Main window hint: "Say “Hey ‹Name›” and your request, type one, or hold Talk."
- Phone app and web page: "The PC is listening for its name." (no change to the remote protocol).
- `IntentRouter`'s leading filler also takes off the chosen name (`hey jarvis, open firefox` → the fast path), given
  to the router by `AppController` (`IntentRouter::setAssistantName`).
- README's voice paragraph; CLAUDE.md (runtime, layout, the privacy rule above); ARCHITECTURE.md (M16 row, deviations).

### 3.9 `scripts/download-sherpa.sh` and `--hear`

- `scripts/download-sherpa.sh`: downloads the pinned library and model archives, checks their SHA-256 (§2), and puts
  `lib/libsherpa-onnx-c-api.so` + `lib/libonnxruntime.so` and `kws/{encoder,decoder,joiner}-…int8.onnx`,
  `kws/tokens.txt`, `kws/bpe.model` under `~/.local/share/nexa/sherpa/`. No sudo; `--uninstall` removes the folder.
- `nexa-assistant --hear <wav>`: for that run only, the microphone is replaced by the recording (played in real time)
  and the wake word is on — the whole wake → request → reply → follow-up path runs as if the room had said it. With
  `--screenshot`, the picture is taken once the pipeline is idle.

### 3.10 Choosing the cloud voice (Settings → Voice)

- **Recognition**: the mode stays (Automatic — cloud, offline if it fails / Cloud only / Offline only); new **Cloud
  provider** list: *Automatic* (the first choice, then every other provider with a recognition model, in the list's
  order — §3.3) and then every provider that exists, the ones that cannot do it greyed with the reason; new **Model**
  list, filled from the chosen provider's model list (names with `whisper`, `transcri` or `audio`), typing allowed.
  With *Automatic*, a line says what is used ("Using Groq — whisper-large-v3-turbo").
- **Speech output**: the mode stays (Automatic / Cloud only / Piper / Speech Dispatcher / Off); new **Cloud provider**
  and **Model** lists the same way (names with `tts`, `speech`, `orpheus`, `playai` or `audio`), and a **Voice** list
  with the voices known for that model, typing allowed — Orpheus English: autumn, diana, hannah, austin, daniel,
  troy (checked live once the terms are accepted); OpenAI `tts-1`/`gpt-4o-mini-tts`: alloy, ash, ballad, coral, echo,
  fable, nova, onyx, sage, shimmer. Piper's voice, the rate and Test voice stay.
- **Greyed reasons**: "no API key"; "takes text only" (agy); "no speech models" (opencode); "Nexa's cloud voice speaks
  the OpenAI audio API" (Gemini and Anthropic kinds).
- **Data**: `config.voice` gains `sttProvider` and `ttsProvider` (a provider id; empty = Automatic). The model and the
  voice stay **per provider** (`ProviderConfig::sttModel`, `ttsModel`, `ttsVoice`), so switching back and forth keeps
  each provider's choice; the Providers tab loses its three speech fields, which now live here.
- **One selection for everything that listens or speaks**: `VoiceBackends` resolves the providers from the config (pure,
  tested: `VoiceBackends::recognitionProviders(config)`, `VoiceBackends::speechProvider(config)`) — used by the PC's
  voice, the phone app and the web remote (`RemoteBridge`), so they always agree.
- **Groq's speech output** needs its terms accepted by the owner in Groq's console; until then a test or a reply falls
  back to Piper and Test voice shows Groq's message.

### 3.11 Microphone check

- **`MicrophoneCheck`** (pure, `voice/capture/`): over the audio the pipeline already receives, a rolling 3 s share of
  clipped samples (|x| ≥ 32 700) and the noise floor (the VAD's).
- **Overloaded** — more than 5 % clipped for 3 s, or the floor above −30 dBFS for 10 s: a status-bar notice, at most once
  a minute: "The microphone is overloaded (52 % of the sound is clipped), so Nexa cannot tell when you speak. Lower the
  input volume in GNOME Settings → Sound → Input." Settings → Voice shows the same as a live line ("Microphone: fine" /
  "overloaded"), and the core's hint while the wake word is on says so.
- Nexa never changes the system's input volume itself.

## 4. Error handling

- Spotter library or model missing, or a function missing (another version): the second net works alone; Settings
  says why; one status-bar notice when the wake word is switched on.
- Spotter fails to start (bad model file): the same, with the library's message (it prints to stderr — captured into
  `nexa.voice` debug logs only).
- The name gives no usable keyword lines (e.g. only digits): Settings refuses it ("use letters").
- Groq unreachable, out of quota or refusing: `RecognizerChain` falls back to offline whisper and skips Groq for a
  minute (unchanged behaviour).
- A request that comes back empty: "I didn't catch that." (unchanged).

## 5. Testing

Pure, Catch2:
- `WordPieces`: a model file built in the test (pieces + scores) and the 22 splits recorded from sentencepiece in the
  probe (their pieces and scores copied from the real `bpe.model`); a BPE-type file is refused.
- `WakePhrases`: lines for "Nexa", "Jarvis", "Mister Robot"; sound-alike rules; labels without spaces; sensitivity
  values; names refused.
- `WakeWordDetector`: greeted anywhere ("I was just saying that, hey Nexa, pause the music" → "pause the music"),
  misheard after a greeting ("Hey Nexus", "heynecks", "OK Neksa"), bare only at a clause start, and negatives ("Next
  song please", "The Nexus phone is really good", "I love Jarvis movies", "an extra hour"); the command and the word
  index; custom names.
- `Config`: the four keys round-trip; defaults when missing.
- `VoiceBackends`: first choice without a speech model → the next provider with one → whisper.
- `IntentRouter`: a typed "hey jarvis, open firefox" reaches the fast path when the name is Jarvis.
- The cloud voice selection: an explicit provider; Automatic's order; a provider without a key, agy, opencode and a
  Gemini kind greyed with their reasons; the voice table; `sttProvider`/`ttsProvider` round-trip.
- `MicrophoneCheck`: clean speech-like PCM → fine; 52 % clipped → overloaded; a loud steady floor → overloaded; the
  notice at most once a minute.

`VoicePipelineTest` (fake input, fake spotter, fake recognisers):
- A spotter hit mid-speech: Listening at once; the request recogniser gets the audio from 0.3 s before the phrase,
  nothing earlier; "Hey Nexa, open YouTube" back from it becomes the request "open YouTube".
- A bare-name hit without a pause before it is ignored; a greeted one is not.
- Second net: the name mid-utterance → the request recogniser gets the audio from the word time; its failure → the
  offline text after the name.
- An utterance arriving while whisper is busy is checked afterwards, not dropped.
- A 1.2 s pause inside a request does not end it at Normal; 1.4 s does.
- Follow-up: speech starting within 8 s of the spoken reply → a request without the name; silence → Idle; none after
  a typed request; speech already going on when the window opens, or more than 8 dB quieter than the last request, ignored.
- A request started by the name ends at 15 s when the room never goes quiet.
- A hit while speaking stops the speech; a hit while a sentence with the name is being spoken is ignored.

End to end (scripted, then live):
- `--hear` with probe clips (start, mid-sentence, bare, noise, TV) on a private test copy: the request reaches the chat
  through Groq; the clips without the name never wake it.
- Live with the owner: the microphone at a level where the check says "fine"; Groq recognising and Groq speaking (after
  the terms are accepted); the name normally, mid-sentence and with the TV on; a pause mid-request; "Pehchan" and "Ammi
  ko call karo"; a follow-up; interrupting by name; a new name chosen in Settings working at once.

## 6. Risks and open points

- **Roman Urdu**: whether Groq's whisper writes Urdu in Latin letters with the prompt alone is unverified (no Urdu
  speech here); if not, the model still understands Urdu script, and the live test decides whether more is needed.
- **TV and invented names**: neither detector is good with a loud TV and an invented name (§2). Settings' hint for the
  wake name says a distinctive two- or three-syllable name works best.
- **Library version**: the header is vendored from v2.0.1 and the script pins v2.0.1; a later library is only used
  through the script after re-checking the header.
- **Memory**: onnxruntime and the model add about 40 MB to Nexa while the wake word is on.
- **Continuous background speech** keeps the energy VAD in speech, so a request said over a TV ends at the 15 s cap
  instead of at the owner's pause; the follow-up window's onset and level checks can still let a loud TV in.

## 7. Not in this milestone

Speaker recognition (waking only for the owner's voice), an Urdu voice for spoken replies (Piper has none), wake
detection on the phone, echo cancellation (barge-in without the name still wants headphones), and the Tailscale work
(next).
