# M17 — sign in from inside Nexa, a Codex provider, and a catalogue of every key-based provider

Status: design, approved in conversation on 2026-10-31 ("yes and then build and run this project"). Written for the
owner's review; the build proceeds on the owner's say-so, so this is a record rather than a gate.

## 1. What the owner asked for

- "Providers connect like Antigravity auth, Kiro, Grok, Codex, Claude Code … Nexa should authenticate like OmniRoute"
  — "just my assistant should have all the authentication methods OmniRoute's providers section has".
- Earlier the same day: **Sign in** buttons for agy and opencode inside Nexa — no terminal, no gateway — and Codex added
  as a provider the same way (memory note `in-app-signin-request`).
- Chosen scope: **both** — sign-in and the catalogue — then build, test and run.
- Catalogue UI: **both** a searchable gallery and a grouped Add menu; the full list, not a curated one.

## 2. What is in and what is out

OmniRoute's providers section is two things:

- **Key-based, local and no-auth providers** (about 185 of its 276 registry ids can be driven by Nexa's three wire
  formats). In: the catalogue (§5).
- **OAuth providers that reach a coding-agent subscription** (Claude Code, Codex, Antigravity, Cursor, Kiro, Copilot,
  Grok CLI, …). OmniRoute does this by presenting itself as the vendor's own app with the vendor's token. Out, as a
  method. In its place, the part the owner actually wants — **click Sign in, a browser opens, signed in** — through the
  vendor's *own program*: Nexa starts the program, shows its login link and reads its status; the program holds the
  token and makes the requests (§3). Nexa never reads, stores, logs or shows a token.

Decisions:

- **Claude is left out of the sign-in work.** Anthropic's published terms say they do not permit third-party developers
  "to offer Claude.ai login into their own applications, or to route requests through Free, Pro, Max, or Ultra plan
  credentials on behalf of their users" (code.claude.com/docs/en/legal-and-compliance, fetched 2026-10-31); a carve-out
  lets an end user sign in to the unmodified Claude Code binary, but a "Sign in with Claude" button inside Nexa is the
  nearest thing to the prohibited case. Claude stays available through the **Anthropic API key** preset, which the
  catalogue keeps. If the owner wants the `claude` program route knowingly, it is a separate, explicit request.
- **opencode shows only its own built-in sign-ins.** The owner's opencode loads a community plugin
  (`opencode-antigravity-auth`) that adds a Google login Google did not make. Nexa lists the providers a clean opencode
  offers by itself (an allow-list read from a sandboxed server, held as data and tested) and ignores plugin-added ones.

## 3. Sign-in (`core/ai/`)

One interface, three implementations (Strategy; each also a Facade over a short-lived program or server).

```cpp
// core/ai/SignIn.h
struct SignInStatus {                     // never an email, a token or a code in `detail`
    enum class State { Unknown, SignedOut, SignedIn };
    State state{State::Unknown};
    QString detail;                       // "Google AI Pro", "ChatGPT · Free"
};
struct SignInMethod { QString id; QString label; bool needsCode{false}; };

class ISignIn : public QObject {
public slots:
    virtual void checkStatus() = 0;                    // -> statusChecked
    virtual void listMethods() = 0;                    // -> methodsListed
    virtual void signIn(const QString &methodId = {}) = 0;   // -> linkReady …, then signedIn / failed
    virtual void submitCode(const QString &code) = 0;  // only for a method with needsCode
    virtual void signOut() = 0;                        // -> signedOut / failed
    virtual void cancel() = 0;
signals:
    void statusChecked(nexa::SignInStatus);
    void methodsListed(QList<nexa::SignInMethod>);
    void linkReady(QUrl url, QString instructions, bool browserOpened);
    void signedIn(nexa::SignInStatus);
    void signedOut();
    void failed(nexa::Error);
};
```

One operation at a time; `cancel()` ends the program (process group) and emits `failed(Cancelled)`.

- **`AgySignIn` + `AgyAcp`** (pure: requests, decoding, `parseInitialize`, `parseSession`, `isAuthRequired`). Runs
  `agy acp` with the same private home and workspace `AgySession::prepare` makes. *Status*: `initialize`, then
  `session/new {cwd, mcpServers:[]}` — error `-32000 "Authentication required"` means signed out, a result carries
  `_meta.antigravity.tier.name` (the plan) — then `session/close`. *Sign in*: `authenticate {methodId:"oauth-personal"}`;
  agy opens the browser itself (`browserOpened = true`), the reply comes after the callback, so the wait is long
  (5 min) and cancellable. *Sign out*: `logout` (only when `agentCapabilities.auth.logout` is present), behind a
  confirmation. The sign-in lives in the **system keyring**, shared with the owner's terminal agy, so signing out
  here signs out there too — the confirmation says so. A locked or unreachable keyring looks like signed out; the
  status text says "not signed in, or the keyring is locked". Anything unparseable is `failed(Parse)`, never "signed
  out". `acp-server-*.log` files hold the account email and OAuth state: never read, never shown.
- **`OpenCodeSignIn`**, on `OpenCodeServer::ensureStarted()` and `request()` unchanged. `GET /provider/auth` →
  `{providerId: [{type:"oauth"|"api", label, prompts?}]}`; the methods offered are the allow-listed providers' *oauth*
  entries (id = `<providerId>#<index>`). `POST /provider/{id}/oauth/authorize {method, inputs?}` →
  `{url, method:"auto"|"code", instructions}` or a `null` body (not an oauth method — treat as failure). `auto`:
  Nexa opens the URL, then holds `POST …/oauth/callback {method}` until it returns `true`; `code`: the dialog asks for
  the pasted code, `submitCode` posts `{method, code}`. One pending flow per provider; a new authorize replaces it.
  Credentials are written by opencode to `~/.local/share/opencode/auth.json` (shared with the terminal opencode).
  Status = `GET /provider/auth` for the sign-in list plus the server's own list of connected providers (`GET
  /provider` `connected`). Sign out = `DELETE /auth/{id}` if the server has it, else not offered.
- **Codex** (`codex app-server`, newline-delimited JSON-RPC over stdio, no `"jsonrpc"` field):
  - `CodexProtocol` (pure): requests/decoders for `initialize` (`clientInfo.name` = `nexa`, `experimentalApi:false` at
    first) + `initialized`, `account/read`, `account/login/start {type:"chatgpt"}` → `{loginId, authUrl}`,
    `account/login/completed`, `account/login/cancel`, `account/logout`, `model/list`, `thread/start`, `turn/start`,
    `turn/interrupt`, the `item/agentMessage/delta`, `turn/completed` and `error` notifications.
  - `CodexSignIn`: *status* = `account/read` (`account:null, requiresOpenaiAuth:true` = signed out; `account.type`
    `chatgpt` + `planType` = signed in; detail = plan, not email); *sign in* = `account/login/start`, `linkReady(authUrl,
    …, browserOpened=false)` and Nexa opens it, then waits for `account/login/completed {success}`; *sign out* =
    `account/logout`. Codex runs the loopback callback itself.
  - `CodexProvider` + `CodexSession` + `CodexEvents`, modelled on `AgyProvider` / `AgySession` / `AgyEvents`, reusing
    `TurnLedger`, `TranscriptWriter` and `TextToolParser`. One warm `codex app-server` per conversation, own session
    and process group; thread started `ephemeral:true`, `approvalPolicy:"never"`, `sandbox:"read-only"`, a private
    `cwd` (`~/.cache/nexa/codex/work`), Codex's own tools off (`--disable shell_tool` and the other stable feature
    flags, `web_search="disabled"`), `model` always explicit. Nexa's tools go as text (`nativeTools = false`) as with
    agy; the native `dynamicTools` route is a later step, after one live turn proves it. `listModels` checks
    `account/read` first and fails `Unauthorized` when signed out (an unauthenticated `model/list` returns a bundled
    catalogue, not the account's). Cancel = `turn/interrupt`, with the process group killed if no `turn/completed`
    arrives within 3 s; after an interrupted or failed turn the ledger resets. Errors map to `ErrorCode`
    (`Unauthorized`, quota → what `meansUnavailable` already chains on). Kind `codex`
    (`kKindCodex`), program found by `findProgram("codex")`; not a default, not a fallback by itself.
- `ProviderRegistry::createSignIn(const ProviderConfig &)` returns the right `ISignIn` for the program kinds (agy,
  opencode, codex) and `nullptr` otherwise; it shares the `OpenCodeServer` the provider already uses.

Not verified until a real sign-in (to be checked by the owner's first click, with `QT_LOGGING_RULES="nexa.ai.debug=true"`):
agy's `authenticate` reply shape and error text on cancel/timeout; Codex's `account/login/completed` live; opencode's
`auto` callback for ChatGPT. Unit tests use fakes only — **no test starts the real agy, opencode, codex or claude, and
none signs in or out of anything.**

## 4. Settings → Providers

- A program-backed provider's form gets a **status line** ("Signed in · Google AI Pro" / "Not signed in" / "Checking…"
  / "Not signed in, or the keyring is locked"; word-wrapped label, colour from `theme.qss`, never alone), **Sign in…**
  and **Sign out** buttons. Sign in with several methods (opencode) asks which in a small menu; `needsCode` methods
  show the link and a code field. The link is always shown and copyable, whether or not the browser was opened.
  Sign out asks first, and for agy says the terminal agy is signed out too. The status re-checks when the tab is shown
  and after sign-in (the `BrowserSettingsPage` poll pattern).
- When a program provider answers `Unauthorized`, `AppController` no longer raises the API-key prompt for it; the chat
  says "Sign in under Settings → Providers" and the dialog can be opened there.
- **Catalogue UI**: the Add button's menu is grouped into category submenus, ending with **Browse all…**, which opens
  `ProviderGalleryDialog` — a search box, category chips and a grid of cards (name, "key" / "no key" badge, host,
  "free tier" mark); picking a card calls the existing `addProvider(preset)`. Both read `availablePresets()`.

## 5. The catalogue (`core/ai/ProviderCatalog`)

- Source: OmniRoute 3.8.51's registry (`open-sse/config/providers/registry/*/index.ts`, 276 ids) joined by id with its
  metadata (`src/shared/constants/providers/apikey/*.ts`, `noauth.ts`, `local.ts`: name, website, hints, free-tier
  flags). MIT-licensed; the notice goes in `THIRD_PARTY_NOTICES.md`.
- A dev-time script, `scripts/generate-provider-catalog.py`, bundles the registry with esbuild and Node, filters it and
  writes `core/ai/ProviderCatalogData.cc` (plain static data; the app never runs Node). Filter: `format` openai /
  claude / gemini; executor `default` or one of the six judged equivalent; `authType` apikey / optional / none; not
  deprecated, media-only, embedding-only or a duplicate base URL; localhost entries kept as "Local". Excluded ids and
  reasons are printed by the script (91 excluded in the survey: 21 oauth, web-cookie and custom executors, Responses
  format, templated URLs, unusual auth headers).
- URL rules: strip a trailing `/chat/completions` (openai-compatible) or `/messages` (anthropic); models URL = `modelsUrl`
  if given, else the kind's default (`{base}/models`). Auth header: Nexa's providers send `Authorization: Bearer` (and
  `x-api-key` / `x-goog-api-key` for the other two); an entry that needs anything else is excluded.
- Entry = id, name, kind, base URL, models URL, `requiresKey` (false for none / optional-without-key / local), category,
  website, free-tier flag, short hint. No model lists (the live `/models` is the source) and no STT/TTS guesses
  (hand-tuned entries only).
- `ProviderPreset` gains `category`, `website`, `hint`, `freeTier` **at the end** (the aggregate initialisers are
  positional). `presets()` = the owner's programs and the existing hand-tuned entries first (they win on id clash), then
  the catalogue; the new Codex preset sits beside agy.
- Categories: Programs, Gateways, Model makers, Inference hosts, Regional, Cloud platforms, Local, Free.

## 6. Tests

Pure Catch2 tests for every parser and decoder; `tests/fake-agy/agy`, `tests/fake-opencode/opencode` and a new
`tests/fake-codex/codex` extended for sign-in and for a Codex turn; a catalogue test (ids unique and lower-case, URLs
parse as http(s), kinds registered, `requiresKey` false only for local / no-auth / optional, no URL ends in
`/chat/completions`, no key-looking strings, categories known); a gallery filter test (the match logic is pure, in
`core/ai/PresetFilter`); `ProviderRegistryTest` extended (the Codex preset, `createSignIn`, the catalogue appended
after the hand-tuned ones). Debug `-Werror`, ASan/UBSan (`detect_leaks=0`, then `grep "in nexa::"`), and the existing
tests stay green. Verified by eye with `--screenshot-settings`.

## 7. Docs

`ARCHITECTURE.md` Status (M17 row and the sign-in facts), `CLAUDE.md` Layout (`core/ai/`, `app/`), README providers
section, `THIRD_PARTY_NOTICES.md`.

## 8. Risks

- agy's ACP, opencode's server API and Codex's app-server are fast-moving, partly undocumented surfaces (agy updates
  itself): unknown replies are `failed(Parse)`, never silently "signed out"; re-check versions before shipping.
- Shared credentials: agy's keyring entry and opencode's `auth.json` are the owner's terminal sign-ins too.
- Per-start cost: `agy acp` + `session/new` boots a language server (about 3 s); status checks run only when the tab is
  shown, never while a warm turn is busy.
