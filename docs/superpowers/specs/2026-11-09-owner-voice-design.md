# M19 — A voice that listens to its owner

Date: 2026-11-09 · Status: approved by the owner (plan review) · Branch: `m19-owner-voice`

## Why

The owner: "most problems are in its voice — it should listen only to a human, not to noise — and voice control should
be much better". Decided with the owner: **only the owner's voice** (enrolled once), with an "anyone" switch;
interrupting by name without killing work; voice control of the tasks on the mission board (M18); a conversation that
keeps going; spoken status of tasks.

What the code does today (M16–M18, read before this design): barge-in takes any speech onset while Nexa speaks, with
no name and no echo guard, so Nexa can interrupt itself and send its own words as a request; a spotter hit is never
confirmed by the transcript and cancels a running reply; the second net's sound key matches "next", "Max", "Rex" for
"Nexa" and has no self-guard; nothing checks who speaks; whisper is trusted as-is; every "stop / be quiet" is a panic.

## Refinements found while building (M18 changed the ground)

- A new spoken request no longer cancels the reply that runs: with tasks, the old one goes on in the background. The
  pipeline's `interruptRequested` therefore stops only the speech, never a task — so a false wake word cannot kill work.
- "Yes" / "go ahead" and "no" / "don't" approve or deny only while a task waits for the user; otherwise they are an
  ordinary answer to the model.
- The owner's `voice.bargeIn = true` (any speech) becomes `name` (by saying the name): `bargeIn` turns from a switch
  into `name | speech | off`, and `speech` is offered as "with headphones".

## Design


**C1. Speaker verification — "only my voice".**
- `voice/listen/SpeakerVerifier` (Strategy behind `ISpeakerGate`): `enrol(pcm16k)` → embedding;
  `score(pcm16k) → float` cosine against the owner's profile; sherpa `SpeakerEmbeddingExtractor` +
  `SpeakerEmbeddingManager` through `SherpaRuntime` (new symbols resolved **optionally** — a missing model leaves
  verification unavailable, said in Settings, never breaks the spotter). Model:
  `3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx` (25 MB; CAM++ 27 MB as the fast alternative), added to
  `scripts/download-sherpa.sh` with a pinned SHA-256 into `~/.local/share/nexa/sherpa/speaker/`.
- Profile: `~/.local/share/nexa/voice-profile.bin` (0600; the embedding dimension, N averaged embeddings, the model
  name). Enrolment in Settings → Voice → *My voice*: "Enrol" records three prompted sentences (the wake name and
  two requests) through `VoicePipeline::measureLevels`-style capture, shows the match of a fourth sentence, "Forget
  my voice". `voice.listenTo = owner | anyone` (default `owner` once a profile exists; `anyone` before),
  `voice.ownerStrictness = low | normal | high` → threshold 0.40 / 0.50 / 0.60 (tuned on the Piper corpus: the three
  voices lessac / ryan / alan in `.superpowers/scratch/kws/clips` stand in for three people).
- Gate in `VoicePipeline`: every utterance that would become a request **without a key press** — the spotter's
  capture, the second net's utterance, the name-alone request, a follow-up, a barge-in — is scored on the raw ring
  audio first; below the threshold it is dropped with a `nexa.voice` debug line ("not the owner: 0.31") and a
  counter shown in Settings ("ignored 4 other voices today"). Push-to-talk and the Talk button are never gated.
- Tests: `SpeakerVerifierTest` (`[sherpa]`, SKIP without the model: same voice ≥ threshold, other voices below,
  fan clips and TV clip rejected), `VoicePipelineTest` with a `ScriptedGate` (owner / stranger per utterance) for
  each of the five paths.

**C2. The wake hit must be confirmed.** After a spotter hit the request's transcript must contain the name per
`WakeWordDetector::find` within its first three words, or be the name alone (→ await the request); otherwise the
capture is discarded and the interrupted reply is **not** cancelled — so cancelling moves from the hit
(`VoicePipeline.cc:384,399`) to the confirmation. Tighten `soundKey` matching: short keys need an exact match or
the same first consonant and the vowel count (`next`, `Max`, `Rex`, `six` must not match `Nexa`); add the
self-guard to the second net (`:530-551`). Tests in `VoicePureTest` (WakeWordDetector negatives) and
`VoicePipelineTest` (a false hit while Thinking leaves the reply running).

**C3. Whisper is not trusted blindly.** `WhisperRecognizer` fills `TimedTranscript::noSpeechProb` and
`meanTokenProb` (`whisper_full_get_segment_no_speech_prob`, `whisper_full_get_token_p`); `Transcript::looksHallucinated`
(pure) drops `noSpeech > 0.6`, `meanP < 0.4`, the known fillers ("Thank you.", "Thanks for watching", "you"),
and any transcript equal to the initial prompt. A request addressed to Nexa (name heard) whose words are dropped is
answered aloud "Say that again?" once. Tests: `VoicePureTest` additions; `--hear` on the fan clips.

**C4. Nexa must not hear itself.** While Speaking the detector's onsets no longer start a barge-in; barge-in =
the name said by the owner (spotter hit → C1 gate → C2 confirmation), or — new setting `voice.bargeIn = name |
speech | off`, default `name`; `speech` is the old behaviour, labelled "with headphones" — any owner speech. The
second net stays off while Speaking unless the gate passed. `replySaysName_` / `kSelfGuardMs` apply to both nets.
Tests: `VoicePipelineTest` (speech onset while Speaking does nothing in `name` mode; a stranger's name hit is
ignored; the owner's cuts the speech).

**C5. Interrupt without killing work.** `core/intent/PcIntents` `ControlIntent` splits: `stop talking | be quiet |
quiet | shut up | hush | that's enough` → `AppController::stopSpeaking()` (voice `cancel()` of the speech only; the
reply keeps streaming, tools keep running); `stop | cancel | never mind | forget it` → stop the **task that is
speaking or was last spoken to** (M18's `MissionControl::stop(taskId)`), not the others; `stop everything | stop all |
panic | halt` → panic as today. `IntentRouterTest` asserts each phrase's route; negatives ("stop the music" stays
media).

**C6. Voice control of tasks.** `core/intent/MissionIntents` (`IIntentHandler`, after the stops): "pause task
two" / "pause the browser task" / "resume …" / "stop …", "approve" / "yes, do it" / "go ahead" and "deny" / "no,
don't" while a task awaits approval (routed to that task, else "Nothing is waiting for you"), "what are you doing"
/ "status" → a spoken one-liner from `MissionControl::summary()` ("Two tasks running, one needs you: the browser
task wants to submit the form"), "add a task: …" → a new task. Argument shapes asserted in `IntentRouterTest`; a
spoken request still becomes a task when no phrase matches.

**C7. Continuous conversation.** `voice.conversation = off | on` ("Keep the conversation going"): after a spoken
reply the follow-up window is 20 s (configurable `voice.followUpMs`, today's hard 8 s moves to config) and renews
after every exchange for the owner's voice only; it ends on "that's all" / "thanks, that's it" / "go to sleep" or
the silence. The core's state name says "Listening to you" (a new `CoreMode::Conversation` colour token in
`NexaCoreSpec` + `theme.qss`, with its name, so the phone and the page show it too). Tests: `VoicePipelineTest`.

**C8. Spoken status.** `voice.announceTasks = off | voiceTasks | all`: M18's `MissionControl` events (finished,
failed, needs approval) are spoken as one short sentence through the pipeline's `speak`, rate-limited to one per 5 s,
never while a request is being heard. Tests: `MissionControlTest` emits → `AppControllerTest` sees `speak`.

**C9. Settings → Voice** gains: *My voice* (Enrol / Forget, strictness, listen to), *Interrupt Nexa* (by name /
by any speech / never), *Keep the conversation going*, *Announce tasks aloud*; the mock-up's "MIC OK · −21 dB · 0 %
clipped" and "last heard › …" come from `VoicePipeline::microphoneTrouble()` / `MicrophoneCheck::Stats` and a new
`heard(const QString &text, bool accepted)` signal, drawn by M18's core panel.

**C10. Verify.** `--hear` with the owner's own enrolment and recordings (the enrolment UI makes them), the fan and
TV clips, and a barge-in test with the speakers on (Nexa reading a long reply, the owner says "Nexa, stop" — the
speech stops, the task continues); `QT_LOGGING_RULES="nexa.voice.debug=true"` shows each utterance's gate score.
Update the M16/M19 rows of `ARCHITECTURE.md`; commit.

---

