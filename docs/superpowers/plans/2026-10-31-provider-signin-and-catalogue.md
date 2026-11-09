# M17 implementation plan — sign-in, Codex, catalogue

Spec: `docs/superpowers/specs/2026-10-31-provider-signin-and-catalogue-design.md`. Branch `m17-signin-catalogue`.
Execution: inline by workflow, in four waves; files are owned by exactly one unit so units can run side by side.

## Skeleton (done before the waves)

`core/ai/SignIn.h` (the `ISignIn` interface and its value types), `kKindCodex` in `Config.h`, and compile-safe stubs for
every new file already listed in `core/CMakeLists.txt`, `tests/CMakeLists.txt` and `app/CMakeLists.txt` — **units never
edit a CMakeLists**; they overwrite their own stubs.

## Wave 1 — four independent units (disjoint files)

| Unit | Owns | Tests |
|---|---|---|
| **A catalogue** | `scripts/generate-provider-catalog.py`, `core/ai/ProviderCatalog.{h,cc}`, `ProviderCatalogData.cc` (generated), `core/ai/PresetFilter.{h,cc}`, `THIRD_PARTY_NOTICES.md` | `ProviderCatalogTest.cc`, `PresetFilterTest.cc` |
| **B agy sign-in** | `AgyAcp.{h,cc}`, `AgySignIn.{h,cc}`, `tests/fake-agy/*` (additions only) | `AgyAcpTest.cc`, `AgySignInTest.cc` |
| **C opencode sign-in** | `OpenCodeSignIn.{h,cc}`, `tests/fake-opencode/*` (additions only), additions to `OpenCodeWire.{h,cc}` | `OpenCodeSignInTest.cc` |
| **D codex** | `CodexProtocol`, `CodexSession`, `CodexEvents`, `CodexProvider`, `CodexSignIn` (`.h/.cc`), `tests/fake-codex/codex` | `CodexProtocolTest.cc`, `CodexSignInTest.cc`, `CodexProviderTest.cc` |

Rules for every unit: TDD (pure decoders first, test, then the Facade against the fake program); follow `CLAUDE.md`
conventions; no real vendor program is ever started by a test or a probe that signs in or out, and no model prompt is
sent; builds go through `flock /tmp/nexa-build.lock cmake --build build-debug --target nexa_tests -j3` (one build at a
time, 7 GB of RAM); run only the unit's own tests (`./build-debug/tests/nexa_tests "<pattern>"`); never print a token or
key; report exact files changed and test names.

## Wave 2 — wiring and UI (one unit, after wave 1)

- `ProviderRegistry`: `presets()` = hand-tuned entries + Codex preset + catalogue (hand-tuned win on id clash);
  `ProviderPreset` gains `category`, `website`, `hint`, `freeTier` at the end; `registerKind(kKindCodex, …)`;
  `createSignIn(config)`; the Add-menu grouping data; `ProviderRegistryTest` extended.
- `app/SettingsDialog`: the sign-in row (status line, Sign in…, Sign out, the method menu, link + code field), the grouped
  Add menu, "Browse all…"; `app/ProviderGalleryDialog` on `PresetFilter`; `app/AppController`/`MainWindow`: no API-key
  prompt for program kinds, "Sign in under Settings → Providers" instead; colours from `ui/theme.qss`.
- `--screenshot-settings` pictures of the Providers tab and the gallery in both schemes.

## Wave 3 — verification

Full debug `-Werror` build and `ctest`; ASan/UBSan run (`detect_leaks=0`, then `grep "in nexa::"`); an adversarial review
panel over the diff (security: tokens/emails/codes never logged or shown, no real program started by tests, process
groups killed; correctness: state machines, cancel, timeouts; conventions: `CLAUDE.md`); fixes; re-run.

## Wave 4 — docs, release build and run

`ARCHITECTURE.md` Status, `CLAUDE.md` Layout, README; release build; stop the running Nexa, relaunch the new build in its
own app scope ([[restart-nexa-after-fixes]]); a scripted `--new-instance --ask` run offscreen; the owner's sign-in clicks
are the owner's.
