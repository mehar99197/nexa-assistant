# M15 (part A) — agy and opencode pipelines, the provider chain, OmniRoute removed

Status: design, for the owner's review. Part B (Nexa serving an OpenAI-compatible API to other apps) is a
separate spec that builds on this one.

## 1. What the owner asked for

- Use the models the owner already has in **agy** (Google's Antigravity CLI) and **opencode**, on those tools' own
  sign-ins and quotas — no keys copied into Nexa.
- "Also with API": opencode through **its local server API**; **API-key presets** for opencode's hosted API
  (OpenCode Console and OpenCode Go); and later (part B) **Nexa serving an API** to other apps.
- **Remove OmniRoute from Nexa entirely** (preset, defaults, mentions). Today `app/main.cc` re-adds OmniRoute on every
  start when the provider list is empty, which is why an emptied list still says "Cannot reach OmniRoute".
- When the first choice cannot answer, an **ordered chain** of providers takes over, in an order the owner sets.
- Approach chosen: **warm sessions** — a long-lived agy process and a private opencode server, each holding the
  conversation, with only the new turns sent.

Rules that stay (CLAUDE.md): every action goes through `ToolExecutor::execute`; agy's and opencode's own tools never
act; keys and passwords are never logged or shown; nothing blocks the GUI thread; `core` never includes QtWidgets.

## 2. What was measured on this machine (2026-10-27)

**agy 1.4.8** (`~/.local/bin/agy`, a Go binary)
- `agy models` prints `Fetching available models...` and then `id<TAB>display name` lines (six today:
  `gemini-3.8-flash-{high,medium,low}`, `claude-sonnet-4-6`, `claude-opus-4-6-thinking`, `gpt-oss-120b-medium`).
  `--output-format` is **not** accepted by `models` in this version.
- One-shot: `agy -p "<prompt>" --model <id> --output-format stream-json --disable-slash-commands` writes NDJSON:
  `{"event":"init",…,"init":{"model","cwd","tools":[…60 names…],"permission_mode":"request-review"}}`, then
  `{"event":"step_update","step_update":{"step_type":"user_input"|"agent_response"|"tool","state":"ACTIVE"|"DONE"|"ERROR","text_delta":"…","usage":{…}}}`,
  then `{"event":"result","result":{"status":"SUCCESS"|"ERROR","response":"…","error":"…","usage":{…},"denied_actions":[{"action","display_name"}]}}`.
- **Warm**: `agy --input-format stream-json --output-format stream-json --model <id> --disable-slash-commands --print=`
  reads one line per turn — `{"event":"user","message":{"content":[{"type":"text","text":"…"}]}}` — and keeps the
  conversation (a second turn remembered the first). `-p` must not be followed by a flag (it takes the next word as
  the prompt); `--print=` with an empty value is what works. Unknown events are ignored with a warning on stderr.
- Cost: ~6 s to start a process; a warm turn's first text arrived **2.6 s** after the line was written (Gemini 3.8
  Flash low). Every turn carries ~6.8k tokens of agy's own prompt.
- In print mode agy **refuses its own acting tools**: asked to run a command, `run_command` failed with "permission
  check failed", `result.denied_actions` named it, nothing was written — and `response` was **empty**.
- agy has no system-prompt flag and no HTTP API of its own (`remote-control` is for its own remote UI).

**opencode 1.19.3** (`/usr/local/bin/opencode`)
- Nine credentials (two OAuth: GitHub Copilot, Google with the Antigravity plugin); `/config/providers` lists seven
  providers and ~500 models (`opencode` 9, `google` 44 incl. `antigravity-*`, `openrouter` 371, …).
- `opencode serve --hostname 127.0.0.1 --port <p>` with `OPENCODE_SERVER_PASSWORD` in the environment: HTTP basic
  auth (user `opencode`), 401 without it. Prints `opencode server listening on http://127.0.0.1:<port>`; healthy
  (`GET /global/health`) after ~5 s, first prompt handled after ~11 s (providers and plugins load on first use).
- `POST /session` → `{id}`; `POST /session/{id}/prompt_async` (or the blocking `/message`) takes
  `{model:{providerID,modelID}, system, tools:{name:bool}, variant, parts:[{type:"text",text}|{type:"file",mime,url}]}`.
  `GET /event` is SSE: `message.part.delta {sessionID,messageID,partID,field:"text"|"reasoning",delta}`,
  `message.part.updated`, `session.status {status:{type:"busy"|"retry"|"idle", attempt, message, action:{reason}}}`,
  `session.error`, `session.idle`. `POST /session/{id}/abort`, `DELETE /session/{id}`.
- With `system` set and `tools:{"*":false}`: **4,888** input tokens for "say hello" through
  `google/antigravity-gemini-3-flash`, reply in ~7 s. Through `opencode run` (its coding agent): **68,690**.
- The free tier ran out during probing: the session went `retry` with `action.reason:"free_tier_limit"`, "Free usage
  exceeded, subscribe to Go", and **kept retrying silently** — the blocking call hung for 150 s.
- `opencode run` waits for its standard input to close; any child opencode gets `/dev/null` as stdin.

**OpenCode's hosted API** (needs an API key from the OpenCode console)
- Console: `https://opencode.ai/inference/openai/v1/chat/completions`, `…/inference/anthropic/v1/messages`; models at
  `https://opencode.ai/inference/v1/models` (84). Go subscription: the same under `/inference/go/…` (38 models).
  `{base}/models` is 404 there, so the model list needs its own URL. Some models (GPT) are served only on the
  Responses API, which Nexa does not speak; free-tier models answer only inside opencode itself.

## 3. Components

All in `core/ai/` beside the existing providers (QProcess in core has a precedent: `core/mcp/StdioMcpTransport`).

| Class | What it does | Pattern |
|---|---|---|
| `TurnLedger` (pure) | What a backend session already holds, and for a new request: *continue — send these turns* or *fresh — start over*. | — |
| `TranscriptWriter` (pure) | Writes earlier history as one labelled block for a fresh session, and the "updated instructions" preface. | Builder |
| `AgyEvents` (pure) | Decodes agy's NDJSON lines; parses `agy models` output. | — |
| `AgySession` | One warm `agy` QProcess: writes turn lines, reads events, one turn at a time, idle stop. | — |
| `AgyProvider` | `IAiProvider` over one `AgySession`; finds the program; lists models. | Strategy |
| `OpenCodeWire` (pure) | Request turns → `prompt_async` body; `/config/providers` → model ids; SSE event JSON → typed events. | — |
| `OpenCodeServer` | Starts and keeps Nexa's private `opencode serve` (or attaches to a URL); readiness; the shared `/event` stream; stop. | Facade |
| `OpenCodeProvider` | `IAiProvider` over the server; one session per conversation. | Strategy |
| `ProviderChain` | An `IAiProvider` made of the ordered providers; first that can answer; skip window; who answered. | Chain of Responsibility (as `RecognizerChain`) |
| `TextToolParser::StreamFilter` (pure) | Lets a text-tools reply stream to the chat while holding back `<tool_call>` markup. | — |

Supporting changes: `HttpClient::getStreaming` (an SSE GET, like `postStreaming`); `ChatStream::route()` (which provider
and model answered); `ProviderConfig::fallback` and `modelsUrl`; `Config::kVersion` 3; the presets.

Both pipelines report `Capabilities{.nativeTools = false}`: the `Agent` describes Nexa's tools in the system prompt
(`TextToolParser::instructions`), writes earlier calls and results as text (`asText`), and reads `<tool_call>` blocks out
of the reply — the path that exists today for models without function calling.

## 4. TurnLedger — the rules

State per backend session: the model, a hash of the system prompt last sent, and the list of messages the backend
holds, as Nexa sent them, ending in a **reply slot** (the backend's own answer).

For a new request (`ChatRequest` after `asText`, system message first):

1. **Compare messages by role and text only.** Images are not compared (the backend already saw them), and the note
   `Conversation` writes when it drops an older screenshot is removed before comparing (the note's text becomes a
   shared constant in `Conversation`).
2. **The reply slot matches any assistant message.** The backend holds its own reply; Nexa's copy differs (tool calls
   re-rendered compactly by `asText`, a cancelled reply cut short).
3. **The request may start later than the session** — `Conversation` keeps a 48k-character window, so long chats drop
   their oldest turns. The request's history must be a contiguous tail of what the session holds (aligned at the
   latest possible position), followed by the reply slot, followed by the new turns.
4. **Continue** when 3 holds, the new turns are non-empty, contain no assistant message, and the model is the same.
   The new turns become one backend turn (consecutive user turns are already merged by `asText`). If the system
   prompt's hash changed, the turn opens with: "Nexa's instructions have changed; these replace the earlier ones:"
   and the new prompt (agy only — opencode takes `system` with every message).
5. Otherwise **fresh**: a new session whose first turn is the system prompt (agy) or `system` (opencode), then
   `TranscriptWriter`'s block for every history message before the new turns ("Earlier in this conversation —
   User: … / Nexa: … / Tool result (name): …"), then the new turns.
6. After the backend answers, the session holds the request's messages plus a new reply slot. A failed or cancelled
   turn marks the session unusable: the next request is fresh.

## 5. How one request flows

**Chain.** `Agent::sendStep` → `ProviderChain::chat()`. Links are the first-choice provider, then every other provider
with `fallback` on, in list order. The chain tries the first link not in its skip window, using **that link's own
model** (`ProviderConfig::defaultModel`; the request's model for the first choice). It moves to the next link only
when the failure means *this provider cannot answer now* and **nothing has streamed yet**:
`ConnectionRefused`, `NotFound` (program missing), `Unauthorized` (not signed in), `Timeout` (server did not start),
HTTP 429 / 5xx, and opencode's quota retry. A 400-class refusal is passed through — the `Agent`'s image retry depends
on it. A link that failed is skipped for 60 s; one that answers clears its mark. When every link fails, one error lists
each reason ("opencode: not signed in to google — run `opencode auth login`; agy: not installed").

Native and text links can mix: `capabilities()` reports the first link the chain will try. If it then falls to a
text-only link with a request built for native tools, the chain converts it the way the `Agent` would
(`asText`, `instructions(request.tools)` appended to the system message, tools cleared). The reverse needs nothing — a
native model given the convention writes text calls, which the `Agent` already parses.

The stream's `route()` names the link and model, so the reply's header reads `agent · via agy · claude-sonnet-4-6` when
a fallback answered, and the status bar says why (`AI: agy — opencode unavailable: not signed in`).

**agy turn.** `TurnLedger` decides.
- Fresh: start `agy --input-format stream-json --output-format stream-json --disable-slash-commands --print=` plus
  `--model <id>` when a model is chosen (agy's own default otherwise), in a private empty working folder
  (`~/.cache/nexa/agy`, 0700), stdin a pipe, its own process group (`CreateNewSession`), killed with the group.
- Write the turn line; `text_delta` of `agent_response` steps go out as `textDelta`; `result` `SUCCESS` finishes;
  `ERROR` fails with its `error`.
- An empty `response` with `denied_actions`: agy's model reached for its own tool. Nexa sends one more turn — "Use only
  Nexa's tools, written as <tool_call> blocks as described" — and if that comes back empty too, the step fails with
  "agy tried to use its own <tool> tool, which Nexa does not allow".
- Cancel kills the process (the next request is fresh). Ten idle minutes stop it. A process that exits before its
  first event fails the turn with what it printed (e.g. not signed in: "run `agy` once in a terminal to sign in").
- Images: an `image` content block will be tried during implementation. If agy does not take one, the provider
  reports `vision = false` and the `Agent` sends the words with a note, as it does for other text-only models.

**opencode turn.**
- Server: started at Nexa's start when opencode is the first choice (so the ~11 s is not paid by the first question),
  and the first time the chain reaches it when it is only a fallback (that request waits for it):
  `opencode serve --hostname 127.0.0.1 --port 0` with the port read from its "listening on" line (measured: it takes
  4096 when free, so the line is the only reliable source), stdin `/dev/null`, a random 32-byte hex password in
  `OPENCODE_SERVER_PASSWORD` only, a private working folder (`~/.cache/nexa/opencode`) so no project's `AGENTS.md` or
  files are picked up, `OPENCODE_CONFIG_CONTENT` denying its edit, bash and web permissions as a second lock, own
  process group. Ready when the "listening on" line has appeared and `/global/health` answers 200 (30 s limit). One
  `/event` stream per server, reconnected if it drops, dispatched by `sessionID`. Stopped with Nexa.
- Attach instead: when the provider's URL is set, Nexa uses that server and its password (the provider's key field).
- Turn: `TurnLedger` decides fresh (`POST /session`) or continue, then `POST /session/{id}/prompt_async` with `system`,
  `model` (`provider/model` split at the first `/`), `tools:{"*":false}`, the turn as a text part and images as file
  parts (data URLs). `message.part.delta` with `field:"text"` for the session's assistant message → `textDelta`
  (reasoning is not shown); `session.idle` finishes; `session.error` fails (an auth error says
  `run opencode auth login`).
- `retry`: a quota or free-tier reason, or retrying for more than 30 s, aborts the session and fails as "cannot
  answer now" — the chain moves on.
- Cancel: `POST /session/{id}/abort`; the session stays, the ledger marks it unusable.

**Streaming text-tools replies.** Today a text-tools reply appears only when complete, so the markup never flashes.
`StreamFilter` passes text through as it arrives and holds back from the first `<tool` (or a partial `<`, `<t`, … at the
end of the buffer) until it knows what it is; the calls are still parsed from the whole reply when it ends.

**Models.** agy: `agy models` (cached for the session). opencode: `GET /config/providers` → `providerID/modelID`, all of
them; the empty choice means opencode's own default model. Both fill the toolbar's model list like any provider.

## 6. Presets, config and Settings

Presets (`ProviderRegistry::presets()`), OmniRoute's removed:
- `opencode` — kind `opencode`, no key, empty URL (Nexa's own server), empty model (opencode's default).
- `agy` — kind `agy`, no key, empty program (found on PATH or in `~/.local/bin`), empty model (agy's default).
- `opencode-console` — OpenAI-compatible, `https://opencode.ai/inference/openai/v1`, key,
  models at `https://opencode.ai/inference/v1/models`.
- `opencode-console-claude` — Anthropic, `https://opencode.ai/inference/anthropic/v1`, key, same model list.
- `opencode-go`, `opencode-go-claude` — the same under `https://opencode.ai/inference/go/…`.
- The other presets stay (OpenAI, Ollama, LM Studio, Groq, OpenRouter, DeepSeek, Mistral, xAI, Together, custom,
  Anthropic, Gemini).

Config version 3:
- `ProviderConfig::fallback` (default true): this provider may answer when the ones before it cannot.
- `ProviderConfig::modelsUrl` (default empty = `{base}/models`), honoured by the OpenAI and Anthropic providers.
- For `agy` the URL field is the program's path; for `opencode` the server's URL. `ProviderRegistry::create` asks for a
  base URL only for the HTTP kinds.
- Defaults: on the first start, and once when a version 2 config with **no providers** is upgraded (the owner's case),
  Nexa adds `opencode` and `agy` — whichever is installed — and makes the first one the first choice. After that an
  empty list stays empty ("No AI provider is configured — open Settings"); `main.cc` no longer invents one.
- An `omniroute` entry already in someone's config is left alone; it is an ordinary OpenAI-compatible provider.

Settings → AI:
- "Active provider" becomes **First choice**; the list gains ▲ / ▼ to order the rest.
- The form shows what the kind uses: for agy *Program* (placeholder "agy on PATH"); for opencode *Server* (placeholder
  "Nexa starts its own") and *Server password* (attach only); the speech fields and the "cannot call tools" box only
  for HTTP kinds. New check box: **Use when the providers above cannot answer** (`fallback`).
- **Test connection** lists the models, which for opencode starts the server, and says how many.

Voice: cloud speech comes from the first choice's speech models; agy and opencode have none, so recognition is local
whisper.cpp (`ggml-base.en.bin` is installed) and speech is Piper or speech-dispatcher — the chains that exist.

## 7. Errors

| Situation | What the user sees |
|---|---|
| `agy` / `opencode` not installed | The chain moves on; Settings' test says "agy is not installed (looked on PATH and in ~/.local/bin)". |
| Not signed in | Moves on; "opencode is not signed in to google — run `opencode auth login` in a terminal" / "run `agy` once in a terminal to sign in". |
| opencode server will not start in 30 s | Moves on; its last stderr lines in the log (`nexa.ai`), never the password. |
| Quota / free-tier limit | Moves on; "opencode: free limit reached". |
| agy reached for its own tool twice | That step fails with the tool's name; the conversation continues. |
| Server or process dies mid-reply | The step fails with what was received kept (no retry: a tool call may have been parsed). Next request is fresh. |
| Every link fails | One message with each provider's reason; the core shows Offline. |

## 8. Security

- The opencode password is random per start, in the child's environment only, never in a log, the config or a
  command line; the server listens on 127.0.0.1 only. Attach mode keeps its password in the provider's key field
  (config.json is 0600).
- Nexa never reads agy's or opencode's credential files; it only runs the programs.
- agy's acting tools are refused in print mode (measured); opencode's are switched off per message and denied by
  config. The system prompt says to use only Nexa's tools. Every action still passes `ToolExecutor` (policy, confirm,
  audit).
- Both run in empty private folders and their own process groups, killed with Nexa (`QCoreApplication::aboutToQuit`
  and `SIGTERM` handling as for commands).
- To verify during implementation: whether agy's **read** tools (`view_file`, `list_dir`, `read_url_content`,
  `search_web`) run without asking in print mode. If they do, a model could read files outside the audit; the plan
  then adds `--sandbox` or an agy agent without tools, and the spec's risk note is updated.
- The Google sign-in inside opencode comes from a third-party plugin (`opencode-antigravity-auth`); Nexa only drives
  opencode and adds no access of its own. agy is Google's own client.

## 9. Testing

Pure (Catch2, one file per class): `TurnLedgerTest` (continue, fresh, the reply slot, trimmed tails, image notes,
model change, changed instructions, repeated identical turns), `AgyEventsTest` (every event above, partial lines,
`denied_actions`, `agy models` output), `OpenCodeWireTest` (bodies, provider list, every event kind, other sessions'
events ignored), `TextToolStreamFilterTest`, `ProviderChainTest` (fake providers: order, skip window, no failover after
output, 400 passed through, native→text conversion, all-failed message), `ConfigStoreTest` (v3, the upgrade with an
empty list, an existing `omniroute` entry kept), `ProviderRegistryTest` (no OmniRoute, presets, `modelsUrl`).

Against stand-ins: `tests/fake-agy/agy` (Python: `models`, the warm stream-json loop, scripted replies, a denied tool,
a crash) drives `AgySession`/`AgyProvider`; `tests/fake_opencode_server.py` (sessions, `prompt_async`, SSE, retry,
error, abort, basic auth) drives `OpenCodeServer`/`OpenCodeProvider`, started as a fake `opencode` executable.
`AgentTest` gains a run through the chain with a text-only fake that writes a `<tool_call>`.

Live (each a `--new-instance` copy, private config and data folders, `QT_QPA_PLATFORM=offscreen`):
opencode first — "what is 2+2" streams; "open calculator and tell me the volume" runs tools through text calls;
agy first — the same, and a second request reuses the warm process (log shows no new start); opencode stopped by
breaking its server → the reply comes `via agy`; Settings' test for both; `--screenshot` of the toolbar's model list.

## 10. Not in this part

- Part B, Nexa's own OpenAI-compatible API (it will reuse `TurnLedger` with a pool of sessions).
- Native tool calls through opencode (exposing Nexa's tools to it as an MCP server) — opencode would then run the loop.
- OpenCode's Responses-API-only models; speech through agy or opencode.

## 11. Documents

`ARCHITECTURE.md`: Status row for M15, a deviation entry (OmniRoute removed by the owner; the decision list's
"OmniRoute is the default provider" replaced), the AI providers section rewritten for the chain and the two
pipelines; `CLAUDE.md`: the OmniRoute runtime paragraph replaced by agy/opencode notes (sign-in, the private server,
the `nexa.ai` log category), layout lines for the new files; `README.md`: providers section. Code comments that name
OmniRoute are reworded to "a gateway"; the behaviour they describe (empty keep-alive chunks, errors inside the stream,
a bearer token for Anthropic-format gateways) stays, because other gateways do the same.
