# M28–M30 — Shipping Nexa Assistant: the Linux package, the site, the accounts

Date: 2026-11-12 · Status: designed with the owner's four decisions (below); the business choices marked **owner**
are proposed defaults · Branch: `m20-hardcore` (continues) · Companion repo: `mehar99197/nexadownloadmanager`

## Why

The owner: "build now for both Windows and Linux, in .deb and .exe, self-contained packages; then make a subdomain of
nexadownloadmanager.com, make a website and upload its build files — the same procedure as Nexa Download Manager —
and make the assistant server-based with auth."

Decided with the owner (2026-11-12):

1. **Linux first, Windows later.** The assistant's core, voice, remote, UI and phone code are portable, but its PC
   side is Linux-specific (the root helper, D-Bus, the portals, the accessibility bus, wpctl, MPRIS). A Windows version
   is a port with its own backends (SendInput, UI Automation, Core Audio…) and gets its own spec when its turn comes.
2. **One account for both products.** The NDM backend and admin become a two-product server: the same login works in
   Nexa Download Manager and Nexa Assistant, one admin panel, releases, plans and subscriptions per product.
3. **AI through the server as a paid feature.** Besides accounts, plans, downloads and the update feed, the server
   relays chat requests to an AI model under a key the server holds ("Nexa AI"), metered per account.
4. **Engines in the installers, models downloaded on first start.** Qt, whisper.cpp, sherpa-onnx and Piper ship in the
   package; the speech and voice models (about 500 MB) download on first start, with progress, inside the app.

How Nexa Download Manager does it today (read 2026-11-12, `~/Desktop/Work/nexadownloadmanager`):

- **Installers**: CPack — a `.deb` whose whole library closure is bundled by linuxdeploy and an NSIS `.exe` with Qt
  deployed and Authenticode-signed — built by GitHub Actions (`.github/workflows/build.yml`) and published as GitHub
  Releases; the version comes from `project(… VERSION)`.
- **Site**: `ndm-website/` — Node 22 + Express + MariaDB backend (`backend/`), React front end (`frontend/`), a React
  admin (`admin/`), on Hostinger shared hosting (CloudLinux, no root): the API runs as `node src/server.js` kept alive
  by an hPanel cron (`deploy/hostinger/run-api.sh`), listening on a Unix socket that `api-proxy.php` streams `/api/*`
  to (`.htaccess` rewrites); built locally and uploaded by `deploy/build-and-upload.sh` (rsync over SSH) behind
  `deploy/deploy-guard.sh` (only what is on `origin/main`, only forward, one deploy at a time).
- **Releases**: an admin creates a release row and uploads each OS's installer (`PUT /api/admin/releases/:id/artifact/:os`,
  version checked against the file); the download page links `/api/releases/download/:os` (counted, range-capable);
  the app's `UpdateChecker` reads `/api/releases/feed?os=`.
- **Accounts**: email or Google sign-in, 2FA, sessions with rotating refresh cookies; plans Free / Pro ($5, $45 a year)
  / Team ($15, 5 seats) through Stripe; the app signs in by the OAuth device flow (`/api/device/*`: a code shown in the
  app, approved at `/activate` on the site; a device token bound to the machine) and validates a licence
  (`POST /api/license/validate`, heartbeat every 6 h, seats) answered with an **Ed25519 licence token** the app verifies
  against a public key compiled in (`packaging/license-public-key.txt`); AI helpers (`/api/ai/*`) run server-side under
  `ANTHROPIC_API_KEY` for plans that include them.

## Shape

Three milestones, in this order, each with its own plan:

| | What | Where |
|---|---|---|
| **M28** | The self-contained `.deb`, the in-app model download, the release pipeline | this repo |
| **M30** | The NDM server as a two-product server; the assistant's site on `assistant.nexadownloadmanager.com`; the Nexa AI relay | the NDM repo (`ndm-website/`), Hostinger |
| **M29** | The app: sign in with the account, plan gates, Nexa AI as a provider, update notices | this repo |

M30 before M29 because the app's account features need the server's contract live; M28 first because the download
page needs a file and nothing else depends on it.

---

## M28 — The Linux package

**The package.** `nexa-assistant_<version>_amd64.deb`, self-contained: it depends on nothing but glibc and the base
system. Built on an `ubuntu-24.04` runner with Qt 6.8 from `aqtinstall` (modules `qthttpserver`, `qtwebsockets`,
`qtmultimedia`, `qtspeech`; the system's Qt is 6.4, below the project's 6.6 floor), `libpolkit-qt6-1-dev` from apt,
whisper.cpp built static as today (`NEXA_BUILD_WHISPER=ON`). linuxdeploy and its Qt plugin (pinned, checksummed — the
NDM job's recipe) collect the complete library closure of `nexa-assistant` and `nexa-helper` into
`/usr/lib/nexa-assistant/lib` with RPATHs, and the Qt plugins beside them. So the `.deb` installs on Ubuntu 24.04 or
newer and Debian 13 or newer (glibc ≥ 2.39).

**Layout in the package** (CMake `install()` rules; `CPACK_PACKAGING_INSTALL_PREFIX /usr`):

| Path | What |
|---|---|
| `/usr/bin/nexa-assistant` | the app; its RPATH finds the bundled libraries, so no wrapper script |
| `/usr/libexec/nexa-assistant/nexa-helper` | the root helper |
| `/usr/lib/nexa-assistant/lib`, `…/plugins` | Qt and every library, the Qt plugins |
| `/usr/lib/nexa-assistant/sherpa/lib` | `libsherpa-onnx-c-api.so`, `libonnxruntime.so` (the pinned sherpa-onnx 2.0.1) |
| `/usr/lib/nexa-assistant/piper/` | the Piper binary, `piper_phonemize`, `espeak-ng-data` |
| `/usr/share/applications/org.nexa.Assistant.desktop`, `/usr/share/icons/hicolor/scalable/apps/org.nexa.Assistant.svg` | the desktop entry and icon |
| `/usr/share/dbus-1/system-services/org.nexa.Helper.service`, `/etc/dbus-1/system.d/org.nexa.Helper.conf`, `/usr/share/polkit-1/actions/org.nexa.helper.policy`, `/lib/systemd/system/nexa-helper.service` | the helper's wiring, the `Exec=` paths templated to `/usr/libexec/nexa-assistant/nexa-helper` |
| `/usr/share/doc/nexa-assistant/` | `README`, `LICENSE`, `THIRD_PARTY_NOTICES` |

`postinst`: `systemctl daemon-reload`, reload the system bus, `update-desktop-database`; nothing is started (the
helper is D-Bus-activated). `prerm`: stop `nexa-helper.service`. `packaging/install-helper.sh` stays for a source
install (`/usr/local`), and the templated unit and service files are generated from one source for both.

**Where the engines are looked up.** `SherpaRuntime::sherpaDir()` and `VoiceBackends::piperDir()` gain a second
place: the user's data folder first (`~/.local/share/nexa/{sherpa,piper}`, what the download scripts fill), then the
package's (`/usr/lib/nexa-assistant/{sherpa,piper}`, a compile-time `NEXA_ENGINE_DIR`). Nothing else changes for a
source build.

**Models on first start.** `system/setup/`: `EngineAssets` (pure: the manifest of what the voice needs — the whisper
`ggml-base.en.bin`, sherpa's spotter, Silero, GTCRN, speaker, ZipVoice and Vocos models, the Piper voice
`en_US-lessac-medium` — each with its URL, size and SHA-256, the same pins the scripts carry; `missing(dataDir)` says
what is not there yet, and the total size) and `AssetDownloader` (one file at a time into `<name>.part`, resumed with a
`Range` request, verified, renamed into place; progress and errors as signals; never on the GUI thread). Settings →
Voice shows what is installed and a **Download (N MB)** button with a progress bar; the first start of a copy with no
models shows one notice in the chat: "Speech needs about 500 MB of models — download them in Settings → Voice. Until
then the cloud recogniser and voice work when a provider has them." The download scripts keep working for a source
install; they and the C++ manifest pin the same URLs and hashes, and a test reads the scripts to prove it.

**Version.** `project(Nexa VERSION 1.0.0)` for the first published build (**owner**: the number); `--version` prints
it; the feed compares it.

**The pipeline.** `.github/workflows/build.yml` in this repo: on a push to `main` and on `workflow_dispatch` — build,
`ctest` (offscreen; the live tests skip without a bus or models), package, `dpkg-deb --info` and an install smoke test
in a clean container (`apt install ./nexa-assistant_*.deb && nexa-assistant --version`), the `.deb` attached to a
GitHub Release tagged `v<version>`. Publishing to the site is the owner's upload in the admin (M30), as for NDM.

**Tests.** `EngineAssetsTest` (the manifest's pins equal the scripts'; `missing()` on fixtures), `AssetDownloaderTest`
(against `FakeHttpServer`: a full download, a resumed one, a wrong hash refused and the part removed), `SherpaRuntime`/
`VoiceBackends` lookup order (a test with both folders), the CI smoke test. [VERIFIED] bar: the `.deb` from CI installs
on a clean Ubuntu 24.04 container and `nexa-assistant --version` runs; installed on this laptop beside the source build
is not done (it would replace the helper the owner runs).

---

## M30 — The server with two products and the assistant's site

All of this lives in the NDM repo (its `AGENTS.md` conventions, `CONTRACT.md` as the API's single source of truth,
`website.yml` runs the backend suite on MariaDB 11.8) and goes live through `deploy/build-and-upload.sh` with its guard.

**The product dimension.** `product ENUM('ndm','assistant') NOT NULL DEFAULT 'ndm'` added (`addColumnIfMissing`) to
`releases` (unique `(product, version)`; `is_latest` per product), `subscriptions` (unique `(user_id, product)`: an
account holds one subscription per product; `Subscription.current`, the trial and the lazy lapse are per row as
today), `device_codes` and `device_tokens` (which app signed in), `payments` and the Stripe metadata (the product's
own prices). `config/plans.js` becomes `PLANS[product]` and `entitlementsFor(product, plan)`; the licence token's
claims gain `product` and its `features` are that product's. Every route that reads a release, a plan or an
entitlement takes the product: `?product=` on the public release routes (default `ndm`, so every existing client keeps
working), the body on device and licence routes (the assistant sends `product: 'assistant'`; absent means `ndm`), the
admin's release and subscription pages get a product selector. The seat, sharing and abuse rules apply unchanged per
subscription.

**The assistant's plans** (**owner**; proposed):

| Plan | Price | What it adds for the assistant |
|---|---|---|
| Free | 0 | the whole assistant on the user's own providers and programs, local voice, the phone, the web remote at home, every theme |
| Pro | $5 a month, $45 a year | **Nexa AI** (1,000 requests a month), **Speak in my voice**, **Away from home** (the tunnels) |
| Team | $15 a month, $135 a year, 5 seats | everything in Pro for five people, 1,000 Nexa AI requests a month each |

Entitlements for the assistant: `{ nexaAi: bool, nexaAiMonthlyRequests: int, ownVoice: bool, awayFromHome: bool,
seats: int }`; Free gets `nexaAi:false, ownVoice:false, awayFromHome:false, seats:1`.

**Nexa AI — the relay.** `routes/assistant.js` → `/api/assistant/v1`, an OpenAI-compatible surface so the app's
existing `OpenAiProvider` speaks to it unchanged: `GET /models` (the models the server offers, from env:
`ASSISTANT_AI_MODELS` = `nexa-fast:<upstream model>,nexa-smart:<upstream model>`), `POST /chat/completions`
(streaming SSE, pass-through of messages, tools and images; the model name mapped; `max_tokens` capped; the request
body bounded). Auth: `Authorization: Bearer <licence token>` (Ed25519-verified, `product:'assistant'`,
`features.nexaAi` true) — the same token the app already holds and renews on every heartbeat. Metering:
`ai_usage(user_id, product, month, requests, prompt_tokens, completion_tokens)` counted on every call (the upstream's
usage object when streamed, else estimated at 4 characters a token); over the month's cap the relay answers
`429 AI_QUOTA_EXCEEDED` with the reset date, which the app shows in words. Upstream: any OpenAI-compatible API from
env (`ASSISTANT_AI_UPSTREAM_URL`, `ASSISTANT_AI_UPSTREAM_KEY`; the owner's Groq key, or OpenRouter for Claude and GPT
models), 60 s idle timeout, the upstream's errors folded to `502 AI_UPSTREAM`. The prompts are the user's own here, on
purpose: the entitlement gates *who* may ask and *how much*, the quota bounds the bill, and the token proves the plan
— the opposite trade from NDM's fixed-prompt helpers, which is why the quota is not optional. `api-proxy.php`
already streams chunk by chunk, so SSE passes through.

**The site.** `ndm-website/assistant/` — a third Vite app (React, the main site's components copied into a shared
`ndm-website/shared/` where both can import them; the NDM look, the assistant's own words and pictures): Home,
Features, Download (reads `/api/releases/latest?product=assistant`, links `/api/releases/download/linux?product=assistant`,
shows the Windows card as "coming"), Pricing (the assistant's plans; checkout on the main site), Docs (the README's
user parts), Privacy and Terms (the main site's), and a header whose Sign in / Dashboard go to `nexadownloadmanager.com`
(one account; the dashboard there shows both products' plans and the Nexa AI usage this month). Served from the
subdomain's directory on the same hosting account with the same `.htaccess` + `api-proxy.php` pair (the API socket is
on the account), so `/api` on the subdomain is the same backend; `CORS_ORIGINS` and the device flow's allowed
origins gain `https://assistant.nexadownloadmanager.com`; `/activate` stays on the main site.

**Hostinger.** The subdomain `assistant.nexadownloadmanager.com` created through the Hostinger API
(`hosting_websites_add-domain`, type `subdomain`) with its own directory; SSL as Hostinger provisions it for
subdomains; the DNS record Hostinger adds itself (the domain's DNS is at Hostinger). `deploy/build-and-upload.sh`
gains the `assistant` component (`SKIP_ASSISTANT=1`), synced to that directory, under the same guard.

**Admin.** Releases: a product selector; the upload accepts a `.deb` whose `control` says `nexa-assistant` and the
release's version. Subscriptions and users: the product shown per row. Root: the Nexa AI usage per account and the
month's totals.

**Tests.** The backend suite gains: product on releases (feed, latest, download, upload — the old routes unchanged
without the parameter), per-product subscriptions (an NDM Pro account is Free in the assistant until it buys),
device sign-in carrying the product, the licence token's `product` claim, the relay (auth, quota, the stream shape,
the upstream mocked), the plans catalog's cards pinned as today. The assistant site's component tests and a
Playwright run against the deployed site, as the main site has.

---

## M29 — The app signs in

- `core/account/`: `AccountClient` (the device flow: `beginSignIn()` → `/api/device/code` with `product:'assistant'`,
  the user code and the `verificationUrlComplete` to open; polling `/api/device/token` at the server's interval;
  `signOut()`; the device token kept in `~/.config/nexa/account.json`, 0600, beside the config — never in a log),
  `LicenseClient` (validate at start and every 6 h, heartbeat; the licence token verified locally — `LicenseToken`, an
  Ed25519 JWT check with OpenSSL's EVP as NDM's, the public key from `packaging/license-public-key.txt` compiled in;
  offline grace as NDM's), `Entitlements` (the assistant's, defaulting to Free), `UpdateChecker` (the feed with
  `product=assistant` and `os=linux`; a status-bar notice "Nexa 1.1 is out — Download" that opens the browser; no
  self-update).
- **Gates** in `AppController`: Speak in my voice, Away from home and Nexa AI switch on only with the entitlement;
  their Settings rows say "Pro" with a link to the pricing page when locked; a plan that lapses turns them off at the
  next validation and says so once.
- **Nexa AI as a provider**: a preset of kind `openai-compatible`, `baseUrl` the relay, `requiresKey:false`, offered in
  Settings → Providers → Add once signed in, its `apiKey` the current licence token set by `AccountClient` at build
  time and on every renewal (never written to `config.json`); models from `/models` (`nexa-fast`, `nexa-smart`);
  `429 AI_QUOTA_EXCEEDED` shown as "Your Nexa AI requests for this month are used up (resets 1 Dec)". The chain treats
  it like any provider.
- **Settings → Account**: Sign in… (the code, large, and a button that opens the page; the app polls), the account's
  plan and Nexa AI usage, Sign out; the status bar's pill shows the plan.
- **Tests**: `LicenseTokenTest` (NDM's vectors), `AccountClientTest` and `LicenseClientTest` against a fake server
  (`FakeHttpServer`: the device flow's states, a token bound to another machine refused, grace), `UpdateCheckerTest`
  (newer / same / malformed feed), `AppControllerTest` (a gate on and off), the Settings tab offscreen.

## Security

- The licence token is Ed25519-signed by the server and verified in the app against the compiled-in public key; the
  device token speaks for the account on this machine only and is stored 0600; neither is logged.
- The relay authenticates every request with the licence token, caps the body, the output and the month's requests,
  never logs a prompt, and reaches the upstream with the server's key only.
- The `.deb` ships the same helper, polkit policy and D-Bus rule as a source install; the helper stays write-only and
  root-only as before.
- The site's `.htaccess`, CSP and the deploy guard apply to the subdomain as to the main site.

## Out of scope

The Windows port (its own spec); self-updating installers; a Pro gate on anything not listed; the Nexa Phone APK on a
store; signing the `.deb`.
