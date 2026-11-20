# M30 — The server with two products and the assistant's site: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The NDM server (`nexadownloadmanager.com`) serves two products — releases, plans, subscriptions, device
sign-in and licence tokens per product — relays Nexa AI for paid assistant accounts, and the assistant gets its own
site on `assistant.nexadownloadmanager.com` deployed by the same script under the same guard.

**Architecture:** A `product` dimension (`'ndm' | 'assistant'`, default `'ndm'` everywhere so every existing client
keeps working) is added to the tables, models and routes that read a release, a plan or an entitlement; the plans
catalogue becomes one per product. `routes/assistant.js` is an OpenAI-compatible relay (`/api/assistant/v1`) that
admits a licence token with `product:'assistant'` and `features.nexaAi`, meters `ai_usage` per month, and streams the
upstream's SSE through. The site is a third Vite app, `ndm-website/assistant/`, importing the main site's
presentational components through a Vite alias; it is served from the subdomain's directory with the same
`.htaccess` + `api-proxy.php` pair (the proxy's socket path adjusted), so `/api` there is the same backend.

**Tech Stack:** Node 22 + Express 4 + mysql2 + Zod (backend, `node --test` suite on MariaDB 11.8), React 19 + Vite +
Tailwind 4 (sites, Vitest + Playwright), bash deploy over rsync/ssh to Hostinger shared hosting, Hostinger API (the
subdomain). All code lives in the NDM repo `~/Desktop/Work/nexadownloadmanager` (GitHub `mehar99197/nexadownloadmanager`).

**Spec:** `docs/superpowers/specs/2026-11-12-ship-and-accounts-design.md` §M30 (this repo).

## Global Constraints

- Work in a **git worktree** of the NDM repo on branch `website/two-products` from `main`: the main checkout holds
  someone's uncommitted work (`routes/admin.js`, `admin.schema.js`, `Subscriptions.jsx`, `LicenseManager.cpp`, …) that
  must not be touched or committed. Commit by path, never `git add -A`.
- NDM conventions (`AGENTS.md`, `CONTRACT.md` §2 envelope `{ok, data}` / `{ok:false, error:{code,message}}`, §11 route
  rules): `ok`/`fail`/`asyncHandler`/`validate`, bcrypt 12, every route change reflected in `CONTRACT.md` §6.
- **Backward compatibility is the acceptance criterion of every route change**: with no `product` given, every
  existing request and response is byte-for-byte what it was (the NDM desktop app and site are live).
- Product ids are exactly `ndm` and `assistant`; names `Nexa Download Manager` / `Nexa Assistant`.
- Assistant plans: Free 0; Pro **$5 monthly / $45 yearly**; Team **$15 monthly / $135 yearly, 5 seats**. Entitlements
  `{ nexaAi, nexaAiMonthlyRequests, ownVoice, awayFromHome, seats }`: Free `false, 0, false, false, 1`; Pro
  `true, 1000, true, true, 1`; Team `true, 1000, true, true, 5`.
- Relay: `/api/assistant/v1`, env `ASSISTANT_AI_UPSTREAM_URL`, `ASSISTANT_AI_UPSTREAM_KEY`,
  `ASSISTANT_AI_MODELS` (`alias:upstream,…`), `ASSISTANT_AI_MAX_TOKENS` (default 4096); errors `401 UNAUTHORIZED`,
  `403 AI_NOT_ENTITLED`, `429 AI_QUOTA_EXCEEDED` (+ `resetsAt`), `502 AI_UPSTREAM`, `503 AI_UNAVAILABLE`; 60 s idle
  timeout; token estimate 4 characters a token when the upstream sends no `usage`.
- Secrets: the upstream key and the server's `.env` are the owner's; never printed, never committed. The deploy
  never touches the main site's live `.htaccess`, `.user.ini`, `api-proxy.php`, `.env` or `uploads`.
- Outward-facing steps (creating the subdomain, deploying, editing the live `.env`) need the owner's go (Task 11).
- Deviations from the spec's sketch, each with its reason, are listed in the task that makes them (T1 signature order,
  T2 no UNIQUE on `(user_id, product)`, T9 alias instead of a `shared/` move).

## Review Focus

1. A client that sends `product` in a case or spelling the enum does not know (`Assistant`, `nexa`) must get `400
   VALIDATION_ERROR`, never silently fall to `ndm` — pinned in T3 (`?product=Assistant` → 400) and T5 (body).
2. An NDM Pro account must be **Free in the assistant** until it buys: the licence route with `product:'assistant'`
   for an NDM Pro key/token must answer `plan:'free'` with the assistant's Free features — pinned in T5.
3. The relay must never meter a request the upstream refused, and never let a request through once the month's
   1,000 are used, under concurrent calls — pinned in T6 (`take` is atomic; a 500 upstream leaves `requests`
   unchanged).
4. A `.deb` of the other product (`Package: nexa` uploaded to an assistant release) must be refused with a message
   naming both packages — pinned in T3.
5. The old update feed must stay byte-identical for NDM (its signature is verified by shipped apps): `buildFeed`
   for `ndm` produces the same `url` as before, with no query string — pinned in T3 (`releaseFeed.test.js`).

---

## File map (NDM repo, `ndm-website/` unless noted)

| Area | Files |
|---|---|
| Products (pure) | `backend/src/config/products.js` (new), `backend/src/config/plans.js`, `backend/src/utils/license.js` |
| Schema + models | `backend/src/config/schema.js`, `backend/src/models/{Release,Subscription,DeviceAuth,TeamMember,Payment}.js`, `backend/src/models/AiUsage.js` (new) |
| Routes | `backend/src/routes/{releases,admin,subscription,webhooks,device,license,user,root}.js`, `backend/src/routes/assistant.js` (new), `backend/src/app.js` |
| Utils | `backend/src/utils/{releaseFeed,artifactVersion,accountPlan,stripe}.js`, `backend/src/utils/sseUsage.js` (new), `backend/src/config/env.js`, `backend/src/middleware/rateLimiter.js` |
| Schemas | `backend/src/schemas/{release,admin,subscription,device,license,user}.schema.js`, `backend/src/schemas/assistant.schema.js` (new) |
| Backend tests | `backend/test/{products,entitlements,planCards,releaseFeed,artifactVersion,licenseToken,stripeCheckoutCustomer}.test.js`, `backend/test/{products,releaseProduct,productSubscriptions,deviceAuth,assistantRelay}.integration.test.js` |
| Main site | `frontend/src/pages/{Activate,Pricing,Dashboard,Billing}.jsx`, `frontend/src/test/products.test.jsx` (new) |
| Admin | `admin/src/pages/{Releases,Subscriptions}.jsx`, `admin/src/pages/AiUsage.jsx` (new), `admin/src/App.jsx`, `admin/src/test/products.test.jsx` (new) |
| Assistant site | `assistant/` (new Vite app: `package.json`, `vite.config.js`, `vitest.config.js`, `playwright.config.js`, `index.html`, `src/**`, `scripts/prerender.mjs`, `e2e/site.spec.js`) |
| Deploy | `deploy/build-and-upload.sh`, `deploy/deploy-guard.sh` (repo root), `ndm-website/deploy/hostinger/assistant.htaccess` (new), `ndm-website/deploy/README.md`, `.github/workflows/website.yml`, `CONTRACT.md`, `AGENTS.md` |
| This repo | `ARCHITECTURE.md` (M30 row, Status), `CLAUDE.md` (the server paragraph) |

---

### Task 1: Products and the plans catalogue per product (pure)

**Files:**
- Create: `backend/src/config/products.js`
- Modify: `backend/src/config/plans.js`, `backend/src/utils/license.js:18-30` (`planSeats`)
- Test: `backend/test/products.test.js` (new), `backend/test/entitlements.test.js`, `backend/test/planCards.test.js`

**Interfaces:**
- Produces `products.js`: `PRODUCTS = ['ndm','assistant']`, `DEFAULT_PRODUCT = 'ndm'`, `PRODUCT_NAMES = { ndm:'Nexa
  Download Manager', assistant:'Nexa Assistant' }`, `DEB_PACKAGES = { ndm: <CPACK_DEBIAN_PACKAGE_NAME of the NDM
  CMakeLists, 'nexa' when unset>, assistant:'nexa-assistant' }`, `isProduct(value) → boolean`,
  `normalizeProduct(value) → 'ndm'|'assistant'` (unknown/absent → `'ndm'`), `productEnum` = the Zod enum shared by
  every schema (`z.enum(PRODUCTS)`).
- Produces `plans.js`: `PLANS` (NDM, unchanged), `ASSISTANT_PLANS` (same card shape: `free {id,name,price:0,features}`,
  `pro {id,name,monthly:5,yearly:45,features}`, `team {id,name,monthly:15,yearly:135,seats:5,features}`),
  `PLAN_CATALOGS = { ndm: PLANS, assistant: ASSISTANT_PLANS }`, `plansFor(product) → catalog`,
  `ASSISTANT_ENTITLEMENTS`, **`entitlementsFor(plan, product = DEFAULT_PRODUCT)`** and `seatsFor(plan, product =
  DEFAULT_PRODUCT)`. *Deviation:* the spec sketches `entitlementsFor(product, plan)`; the plan-first order keeps the
  seven existing callers and every test valid, `product` is the new optional second argument.
- Produces `utils/license.js`: `planSeats(plan, product = DEFAULT_PRODUCT)` (via `seatsFor`); `planExpiry` is
  product-independent and stays.

- [ ] **Step 1: Set up the worktree**

```bash
cd ~/Desktop/Work/nexadownloadmanager && git worktree add -b website/two-products ../nexadownloadmanager-m30 main
cd ../nexadownloadmanager-m30/ndm-website/backend && npm ci --no-audit --no-fund
```
Expected: a clean tree on `website/two-products`; `git status` empty.

- [ ] **Step 2: Write the failing tests** in `test/products.test.js`:

```js
test('the two products and their names', () => {
  assert.deepEqual(PRODUCTS, ['ndm', 'assistant']);
  assert.equal(PRODUCT_NAMES.assistant, 'Nexa Assistant');
  assert.equal(normalizeProduct('assistant'), 'assistant');
  assert.equal(normalizeProduct('Assistant'), 'ndm');   // never a guess
  assert.equal(normalizeProduct(undefined), 'ndm');
  assert.equal(isProduct('nexa'), false);
});
test('the assistant plans are priced as the spec says', () => {
  assert.equal(ASSISTANT_PLANS.pro.monthly, 5); assert.equal(ASSISTANT_PLANS.pro.yearly, 45);
  assert.equal(ASSISTANT_PLANS.team.monthly, 15); assert.equal(ASSISTANT_PLANS.team.yearly, 135);
  assert.equal(ASSISTANT_PLANS.team.seats, 5);
  assert.equal(plansFor('assistant'), ASSISTANT_PLANS); assert.equal(plansFor('ndm'), PLANS);
});
test('assistant entitlements: free is the floor, pro unlocks the three features, team adds seats', () => {
  assert.deepEqual(pick(entitlementsFor('free', 'assistant')), { nexaAi: false, nexaAiMonthlyRequests: 0, ownVoice: false, awayFromHome: false, seats: 1 });
  assert.deepEqual(pick(entitlementsFor('pro', 'assistant')),  { nexaAi: true, nexaAiMonthlyRequests: 1000, ownVoice: true, awayFromHome: true, seats: 1 });
  assert.equal(entitlementsFor('team', 'assistant').seats, 5);
  assert.equal(entitlementsFor('bogus', 'assistant').plan, 'free');
  assert.equal(entitlementsFor('pro').aiRename, true);               // NDM's answer unchanged
  assert.equal(planSeats('team', 'assistant'), 5); assert.equal(planSeats('team'), 5);
});
```
Add to `planCards.test.js`: the assistant's cards mention `Nexa AI`, `1,000`, `Speak in my voice`, `Away from home`
on Pro and Team, and nothing "unlimited".

- [ ] **Step 3: Run** `npm run test -- test/products.test.js` → FAIL (module not found).
- [ ] **Step 4: Implement** `products.js`, `ASSISTANT_PLANS` / `ASSISTANT_ENTITLEMENTS` / `plansFor` /
  `entitlementsFor(plan, product)` in `plans.js` (the NDM `ENTITLEMENTS` lookup unchanged; the assistant's gets
  `plan` and `adFree` the same way), `planSeats(plan, product)`.
- [ ] **Step 5: Run** `node --test test/products.test.js test/entitlements.test.js test/planCards.test.js` → PASS.
- [ ] **Step 6: Commit** `feat(website): two products — the plans catalogue and entitlements per product`.

---

### Task 2: The product column, `ai_usage`, and the models

**Files:**
- Modify: `backend/src/config/schema.js` (releases ≈475, subscriptions ≈228, device_codes ≈798, device_tokens ≈823,
  payments ≈360), `backend/src/models/{Release,Subscription,DeviceAuth,TeamMember,Payment}.js`
- Create: `backend/src/models/AiUsage.js`
- Test: `backend/test/products.integration.test.js` (new)

**Interfaces:**
- Schema: `product ENUM('ndm','assistant') NOT NULL DEFAULT 'ndm'` on `releases`, `subscriptions`, `device_codes`,
  `device_tokens`, `payments` — in the `CREATE TABLE` **and** `addColumnIfMissing(table, "product ENUM(...) NOT NULL
  DEFAULT 'ndm'")` for existing databases; `addIndexIfMissing('releases', 'UNIQUE KEY uq_releases_product_version
  (product, version)')`, `addIndexIfMissing('releases', 'INDEX idx_releases_product_latest (product, is_latest)')`,
  `addIndexIfMissing('subscriptions', 'INDEX idx_subscriptions_user_product (user_id, product)')`. *Deviation:* no
  UNIQUE on `(user_id, product)` — live databases hold several rows per user (admin-issued second licences; the code
  resolves "the newest row" everywhere), so the rule stays "the newest row per (user, product)". New table:
  ```sql
  CREATE TABLE IF NOT EXISTS ai_usage (
    id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY, user_id INT UNSIGNED NOT NULL,
    product ENUM('ndm','assistant') NOT NULL DEFAULT 'assistant', month CHAR(7) NOT NULL,
    requests INT UNSIGNED NOT NULL DEFAULT 0, prompt_tokens BIGINT UNSIGNED NOT NULL DEFAULT 0,
    completion_tokens BIGINT UNSIGNED NOT NULL DEFAULT 0,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uq_ai_usage_user_product_month (user_id, product, month),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE) ENGINE=InnoDB …
  ```
- `Release`: `findLatest(product = 'ndm')`, `findByVersion(version, product = 'ndm')`, `listAll(product = null)`
  (null = every product, newest first), `create({ product = 'ndm', … })` (demotes `is_latest` **within the product**),
  `updateKeepingOneLatest(id, fields)` and `unsetLatestExcept(id)` scoped to the row's own product,
  `sumDownloadCount(product = null)`. `product` is not in `UPDATABLE_COLUMNS` (fixed at creation).
- `Subscription`: `findByUserId(userId, product = 'ndm')`, `findAllByUserId(userId)` (every product, for the admin
  and the account export), `findActiveByUserId(userId, product = 'ndm')`, `create({ product = 'ndm', … })`,
  `updateCurrentByUserId(userId, fields, product = 'ndm')`, `startTrial(userId, now = new Date(), product = 'ndm')`
  (the trial route stays NDM-only in M30: `users.trial_used` is one per account), `count(filter)` + `countByPlan(product
  = null)` accept `filter.product`.
- `DeviceAuth`: `createCode({ product = 'ndm', … })`, `createToken({ product = 'ndm', … })`; `consume` copies the
  code's product onto the token it mints; rows returned carry `product`.
- `TeamMember.findActiveByUserId(userId, product = 'ndm')` adds `AND s.product = ?`.
- `Payment.create({ product = 'ndm', … })` / every `INSERT INTO payments` writes `product`.
- `AiUsage`: `monthKey(date = new Date()) → 'YYYY-MM'` (UTC), `resetsAt(date) → Date` (the 1st of next month 00:00
  UTC), `row(userId, product, month) → {requests, prompt_tokens, completion_tokens} | null`,
  **`take(userId, product, month, limit) → boolean`** (atomic: `INSERT … ON DUPLICATE KEY UPDATE requests = IF(requests
  < VALUES-limit, requests + 1, requests)` implemented as an `INSERT IGNORE` of the month row followed by `UPDATE …
  SET requests = requests + 1 WHERE … AND requests < ?` — `affectedRows === 1` is true), `untake(userId, product, month)` (`requests = requests - 1 WHERE … AND requests > 0`,
  for a request the upstream refused), `addTokens(userId, product,
  month, { promptTokens, completionTokens })`, `listMonth(month) → [{ user_id, email, requests, prompt_tokens,
  completion_tokens }]` + `totals(month)`.

- [ ] **Step 1: Start the test database** (the image is already on this machine):
```bash
docker run -d --name ndm-testdb -p 127.0.0.1:3399:3306 -e MARIADB_ROOT_PASSWORD=ndm-test-root \
  -e MARIADB_DATABASE=ndm_test -e MARIADB_USER=ndm -e MARIADB_PASSWORD=test_password_at_least_16_chars mariadb:11.8
```
Then `npm test` once on the untouched branch → every suite passes or skips, none fails (the baseline).

- [ ] **Step 2: Write the failing test** `test/products.integration.test.js` (boot through `helpers/testServer`):
  - *"the schema upgrades a database that predates the product column"*: drop `product` from `releases` with raw
    SQL, run `initSchema()` again → the column is back with default `'ndm'` and both indexes exist (`indexExists`).
  - *"two releases of the same version, one per product, each the latest of its product"*: `Release.create({version:
    '1.0.0', isLatest:true})` and `Release.create({version:'1.0.0', product:'assistant', isLatest:true})` → both
    rows exist; `findLatest()` is the NDM one, `findLatest('assistant')` the other; a second `create({version:'1.0.1',
    product:'assistant', isLatest:true})` demotes only the assistant's 1.0.0; a duplicate `(product, version)` insert
    rejects.
  - *"a subscription per product"*: a user with an NDM Pro row (`Subscription.create`) → `findActiveByUserId(id,
    'assistant')` is null, `findAllByUserId(id)` has one row; after `create({product:'assistant', plan:'pro'})`, two.
  - *"ai_usage take is atomic and bounded"*: `take(u, 'assistant', m, 3)` ×3 → true, the 4th → false; 20 concurrent
    `take(…, 10)` → exactly 10 true; `addTokens` sums; `listMonth(m)` carries the email; `resetsAt(new Date(Date.UTC(2026,
    10, 14)))` is `2026-12-01T00:00:00.000Z`.
- [ ] **Step 3: Run** `node --test test/products.integration.test.js` → FAIL.
- [ ] **Step 4: Implement** the schema statements, the model signatures above, `AiUsage.js`.
- [ ] **Step 5: Run** `npm test` → all pass (the existing suites prove the defaults keep the old behaviour).
- [ ] **Step 6: Commit** `feat(website): product on releases, subscriptions, devices and payments; ai_usage`.

---

### Task 3: Releases per product — public routes, the feed, the admin, the installer check

**Files:**
- Modify: `backend/src/routes/releases.js`, `backend/src/utils/releaseFeed.js`, `backend/src/utils/artifactVersion.js`,
  `backend/src/schemas/release.schema.js`, `backend/src/schemas/admin.schema.js` (`createReleaseSchema`, a
  `releasesListQuerySchema`), `backend/src/routes/admin.js` (`/releases` GET/POST, `PUT /releases/:id/artifact/:os`)
- Test: `backend/test/releaseFeed.test.js`, `backend/test/artifactVersion.test.js`,
  `backend/test/releaseProduct.integration.test.js` (new)

**Interfaces:**
- Query schemas: `productQuerySchema = { query: z.object({ product: productEnum.default('ndm') }).strict() }` used by
  `/latest` and `/history`; `downloadOsSchema.query` and `feedQuerySchema.query` gain the same `product`.
- `releaseFeed.js`: `downloadRedirectUrl(baseUrl, os, product = 'ndm')` → unchanged for `ndm`,
  `…/download/${os}?product=assistant` otherwise; `buildFeed(release, os, baseUrl)` reads `release.product`. The
  signed field set is unchanged (no `UPDATE_FEED_SIGNATURE_VERSION` bump).
- `artifactVersion.js`: `readDebVersion` and `artifactVersion`/`artifactVersionFromFile` return `{ version, source,
  package }` for a `.deb` (`Package:` from control; `package: null` for PE).
- Admin: `GET /admin/releases?product=` (absent = all; rows carry `product`), `POST /admin/releases` body `product`
  (default `ndm`; the version clash check is per product), upload: when `declared.package` is set and differs from
  `DEB_PACKAGES[release.product]` → `409 PACKAGE_MISMATCH` "This installer is the package `nexa`, but this is a Nexa
  Assistant release (`nexa-assistant`)…" and the stored file is removed, as `VERSION_MISMATCH` does.
- Public responses gain `product` (additive).

- [ ] **Step 1: Failing unit tests**: `releaseFeed.test.js` — `downloadRedirectUrl('https://x', 'linux')` is
  `https://x/api/releases/download/linux` (unchanged) and with `'assistant'` ends in `?product=assistant`; `buildFeed`
  of an assistant row carries that URL. `artifactVersion.test.js` — the fixture `.deb` builder used there gets a
  `Package:` line; `readDebVersion` returns `package`.
- [ ] **Step 2: Failing integration test** `releaseProduct.integration.test.js` (admin client via the helper the
  admin suites use): create `1.0.0` for each product; `GET /api/releases/latest` is NDM's, `?product=assistant` the
  other, `?product=Assistant` → 400 `VALIDATION_ERROR`; `/history?product=assistant` lists only it;
  `/download/linux?product=assistant` 404 `NO_RELEASE` until an upload; upload a tiny generated `.deb` with `Package:
  nexa` to the assistant release → 409 `PACKAGE_MISMATCH`; with `Package: nexa-assistant` → 200, then
  `/download/linux?product=assistant` streams it and `/feed?os=linux&product=assistant` has `version` and the URL
  with the query; `/feed?os=linux` (NDM, no artifact) is still 404.
- [ ] **Step 3: Run** both → FAIL. **Step 4: Implement.** **Step 5: Run** `npm test` → PASS.
- [ ] **Step 6: Commit** `feat(website): releases per product — routes, feed, admin, the .deb package check`.

---

### Task 4: Plans, checkout, Stripe and the account's plan per product

**Files:**
- Modify: `backend/src/routes/subscription.js` (`/plans`, `/checkout`, `/mock-complete`, `/status`, `/cancel`,
  `/resume`), `backend/src/schemas/subscription.schema.js`, `backend/src/utils/stripe.js` (live + mock
  `createCheckoutSession`), `backend/src/routes/webhooks.js` (`planFromObject` + new `productFromObject`,
  `handleCheckoutCompleted`'s row lookup/insert, payment inserts), `backend/src/utils/accountPlan.js`,
  `backend/src/routes/admin.js` (`POST /subscriptions` body `product`, `GET /subscriptions?product=`,
  `/users/:id/details` → `findAllByUserId`), `backend/src/schemas/admin.schema.js`
- Test: `backend/test/stripeCheckoutCustomer.test.js`, `backend/test/productSubscriptions.integration.test.js` (new)

**Interfaces:**
- `GET /api/subscription/plans?product=` → `{ ...plansFor(product), billing }`; `POST /checkout` body `product`
  optional → `stripe.createCheckoutSession({ product, … })`: `product_data.name = `${PRODUCT_NAMES[product]}
  ${catalog.name}``, `metadata.product`, prices from `plansFor(product)`; "already subscribed" is per product;
  `/mock-complete` body `product`; `/status?product=`, `/cancel` and `/resume` body `product` (optional).
- `accountPlan.js`: `subscriptionForUser(userId, product = 'ndm')`, `teamSubscriptionForUser`,
  `ownSubscriptionForUser`, `teamPlanForUser`, `effectivePlanFor` — all with the trailing `product`.
- Webhooks: `productFromObject(obj) = normalizeProduct(obj.metadata?.product)`; the checkout handler locks/creates the
  row of that product; `invoice.paid` resolves by Stripe subscription id first (unambiguous) — the customer-id and
  email fallbacks stay as today and are documented as "ambiguous only for a customer holding both products on an
  invoice without a subscription id, which Stripe does not send".
- Admin: `createSubscriptionSchema.product` (default `ndm`), `listQuerySchema.product` optional, rows carry `product`.

- [ ] **Step 1: Failing tests**: `stripeCheckoutCustomer.test.js` — a checkout with `product:'assistant', plan:'pro',
  billingCycle:'yearly'` names the line item `Nexa Assistant Pro`, `unit_amount` 4500, `metadata.product
  'assistant'`; without `product` everything is as before (`NexaDownloadManager Pro`).
  `productSubscriptions.integration.test.js`: `/plans?product=assistant` has `pro.monthly 5`; `/plans` is NDM's; a
  user mock-completes `{plan:'pro', billingCycle:'monthly', product:'assistant'}` → `/status?product=assistant` is
  `pro`, `/status` is `free`, the payment row has `product 'assistant'`; mock-completing NDM Pro afterwards leaves
  the assistant row alone; `POST /admin/subscriptions {product:'assistant', plan:'team'}` for a user → his
  `/status?product=assistant` is `team` with 5 seats and `/admin/subscriptions?product=assistant` lists it.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** `npm test` → PASS (renewal/billing suites prove
  the webhooks' defaults).
- [ ] **Step 5: Commit** `feat(website): plans, checkout, webhooks and the account's plan per product`.

---

### Task 5: Device sign-in and licence tokens carry the product

**Files:**
- Modify: `backend/src/routes/device.js`, `backend/src/schemas/device.schema.js`, `backend/src/routes/license.js`,
  `backend/src/schemas/license.schema.js`
- Test: `backend/test/deviceAuth.integration.test.js`, `backend/test/licenseToken.test.js`

**Interfaces:**
- `POST /api/device/code` body `product` optional → stored on the code; `GET /device/code/:userCode` answers
  `product` and `productName`; `/token` mints a token with the code's product.
- Licence bodies (`validate`, `heartbeat`, `release`) take `product` optional. Resolution: the device-token path uses
  `req.body.product ?? tokenRow.product`; the key path uses the subscription row's own product (a key is unique
  across products) — a body `product` that disagrees with the key's row → `400 PRODUCT_MISMATCH`.
- The token claims gain `product` and **`uid`** (the account's numeric id, for the relay's metering; `sub` stays
  what it is); `features = entitlementsFor(plan, product)`; the Free fallback answers the product's Free set.

- [ ] **Step 1: Failing tests**: `licenseToken.test.js` — claims `product` and `uid` survive signing. In
  `deviceAuth.integration.test.js` a new sub-test *"the assistant signs in as itself"*: `/device/code {…, product:
  'assistant'}` → `/device/code/:userCode` says `productName 'Nexa Assistant'`; after approval the token's
  `/license/validate {device_token, device_fingerprint, product:'assistant'}` for a user with an **NDM Pro** row
  answers `plan 'free'`, `features.nexaAi false`, `features.aiRename undefined`, and the token decodes with
  `product 'assistant'` and `uid` = the user's id; the same call without `product` on an NDM-issued token stays
  NDM Pro; `product:'nexa'` → 400.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** `npm test` → PASS.
- [ ] **Step 5: Commit** `feat(website): device sign-in and licence tokens per product`.

---

### Task 6: The Nexa AI relay

**Files:**
- Create: `backend/src/routes/assistant.js`, `backend/src/schemas/assistant.schema.js`, `backend/src/utils/sseUsage.js`
- Modify: `backend/src/config/env.js` (`ASSISTANT_AI_*`, `config.assistantAi = { configured, upstreamUrl, models:
  Map<alias, upstream>, maxTokens }`), `backend/src/app.js` (the body parser for the relay before the global one,
  the mount), `backend/src/middleware/rateLimiter.js` (`assistantAiLimiter`: 60 requests a minute per token's
  `uid`, falling back to the IP)
- Test: `backend/test/sseUsage.test.js` (new), `backend/test/assistantRelay.integration.test.js` (new)

**Interfaces:**
- `GET /api/assistant/v1/models` → OpenAI's shape `{ object:'list', data:[{ id:'nexa-fast', object:'model',
  owned_by:'nexa' }, …] }` (no `ok` envelope — an envelope exception recorded in CONTRACT §2 beside
  license-validate's; the app's `OpenAiProvider` reads this shape unchanged). Needs the bearer like the chat route.
- `POST /api/assistant/v1/chat/completions`: middleware `requireNexaAi` — bearer licence token → `verifyLicense`;
  missing/invalid → `401 UNAUTHORIZED`; `payload.product !== 'assistant' || !payload.features?.nexaAi` → `403
  AI_NOT_ENTITLED`; sets `req.ai = { uid, limit: features.nexaAiMonthlyRequests }`. Body (Zod, `.passthrough()` for
  OpenAI's optional fields): `model` ∈ configured aliases, `messages` 1…200 items, `stream` boolean, `max_tokens`
  capped at `config.assistantAi.maxTokens`, `tools`/`tool_choice`/`temperature` passed through; body size 6 MB
  (`express.json({ limit: '6mb' })` mounted on this path **before** the global 1 MB parser in `app.js`).
  Quota first: `AiUsage.take(uid, 'assistant', monthKey(), limit)` false → `429 AI_QUOTA_EXCEEDED` with `details:{
  resetsAt, limit }` and `Retry-After`. Then the upstream `fetch(`${upstreamUrl}/chat/completions`)` with the mapped
  model and `stream_options: { include_usage: true }` when streaming. Non-2xx upstream → `502 AI_UPSTREAM` with
  `details.upstreamStatus` (never its body) **and** the taken request is given back (`AiUsage.untake(...)`:
  `requests = requests - 1 WHERE requests > 0`). Streaming: `Content-Type: text/event-stream`, `Cache-Control:
  no-store`, `X-Accel-Buffering: no`, `flushHeaders()`, pipe the upstream body chunk by chunk, feeding each chunk to
  an `SseUsageReader` to capture `usage` and count streamed content characters; a 60 s idle timer (reset per chunk)
  aborts the upstream and ends with `data: {"error":{"code":"AI_UPSTREAM","message":"…"}}\n\n` then `data: [DONE]`.
  After the stream: `AiUsage.addTokens(uid, 'assistant', month, usage ?? estimate)`. Unconfigured → `503
  AI_UNAVAILABLE` (as `routes/ai.js`).
- `sseUsage.js` (pure): `class SseUsageReader { feed(chunk: string|Buffer); usage → {promptTokens, completionTokens}
  | null; contentChars → number }` (splits on `\n\n`, parses `data:` JSON, reads `usage` and
  `choices[].delta.content`); `estimateTokens(text) = Math.ceil(text.length / 4)`.

- [ ] **Step 1: Failing unit test** `sseUsage.test.js`: chunks split mid-line reassemble; a final chunk with `usage`
  is reported; `[DONE]` ignored; `estimateTokens('abcdefgh') === 2`.
- [ ] **Step 2: Failing integration test** `assistantRelay.integration.test.js`: a fake upstream (`http.createServer`
  in the test, answering `/chat/completions` with three SSE chunks, the last carrying `usage {prompt_tokens: 12,
  completion_tokens: 7}`, recording the request's model, auth header and `max_tokens`; `/models` not needed);
  `process.env.ASSISTANT_AI_UPSTREAM_URL/KEY/MODELS` set before the app loads (`nexa-fast:fake-small`). Tokens are
  made with `signLicenseToken({ sub:'user:7', uid:7, plan:'pro', product:'assistant', device:'a'*32, features:
  entitlementsFor('pro','assistant') })`. Cases: no bearer → 401; an NDM Pro token → 403 `AI_NOT_ENTITLED`; an
  assistant Free token → 403; `GET /models` lists `nexa-fast`; a streamed chat → 200 `text/event-stream`, the body
  is the three chunks verbatim, the fake saw `model 'fake-small'`, `Authorization: Bearer <key>`, `max_tokens` 4096
  when the client asked for 99999, and `ai_usage` now says `requests 1, prompt_tokens 12, completion_tokens 7`; a
  non-stream chat returns the fake's JSON; `model:'gpt-4'` → 400; with `limit` 2 the third call → 429 with
  `resetsAt`; the fake answering 500 → 502 `AI_UPSTREAM` and `requests` unchanged.
- [ ] **Step 3: Run** → FAIL. **Step 4: Implement** (`env.js`, `app.js`, the limiter, the route, the schema).
- [ ] **Step 5: Run** `npm test` → PASS; also `node -e "require('./src/app')"` with no `ASSISTANT_AI_*` set boots.
- [ ] **Step 6: Commit** `feat(website): the Nexa AI relay — /api/assistant/v1 with entitlement and quota`.

---

### Task 7: What the account, the admin and the root read

**Files:**
- Modify: `backend/src/routes/user.js` (`/license?product=`, `/me` adds `products`, new `GET /ai-usage`),
  `backend/src/schemas/user.schema.js`, `backend/src/routes/root.js` (`GET /ai-usage?month=`),
  `backend/src/routes/admin.js` (`/stats` gains `assistant: { subscriptionsByPlan, releases }`)
- Test: `backend/test/productSubscriptions.integration.test.js` (extend), `backend/test/rootTier.test.js` or the root
  integration suite the repo uses for `/root/overview`

**Interfaces:**
- `GET /api/user/me` → `data.user` unchanged, plus `data.products = { ndm: summary|null, assistant: summary|null }`
  (the `subscriptionSummary` shape per product via `effectivePlanFor(userId, product)`).
- `GET /api/user/license?product=` (default ndm).
- `GET /api/user/ai-usage` → `{ product:'assistant', month, requests, limit, remaining, resetsAt }` (limit from the
  effective assistant plan's `nexaAiMonthlyRequests`).
- `GET /api/root/ai-usage?month=YYYY-MM` (default this month) → `{ month, totals, accounts }` from `AiUsage`.

- [ ] **Step 1: Failing tests** in `productSubscriptions.integration.test.js`: `/user/me` for the user above has
  `products.ndm.plan 'free'` and `products.assistant.plan 'pro'`; `/user/ai-usage` after two `AiUsage.take` →
  `requests 2, remaining 998`; `/root/ai-usage` (root client) totals 2 and the account's email.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** `npm test` → PASS.
- [ ] **Step 5: Commit** `feat(website): both products on the account, Nexa AI usage for the account and the root`.

---

### Task 8: The main site and the admin panel show the second product

**Files:**
- Modify: `frontend/src/pages/Activate.jsx` (the product's name from `productName` in place of "Nexa"),
  `frontend/src/pages/Pricing.jsx` (`?product=assistant` → the assistant's plans and checkout; a two-tab switch
  "Download Manager | Assistant" at the top), `frontend/src/pages/Dashboard.jsx` (a "Nexa Assistant" card: plan, Nexa
  AI used/limit this month with `resetsAt`, links to the assistant site), `frontend/src/pages/Billing.jsx` (a product
  column on payments; status per product), `frontend/scripts/prerender.mjs` (unchanged routes)
- Modify: `admin/src/pages/Releases.jsx` (a product selector in the create form, default NDM; a product badge per
  row; the list filter chips All / NDM / Assistant; the installer hint for Linux of an assistant release says
  `nexa-assistant_*.deb`), `admin/src/pages/Subscriptions.jsx` (product filter + badge; product in the issue form),
  `admin/src/pages/AiUsage.jsx` (new, root-only route `/ai-usage`: month picker, totals, the accounts table),
  `admin/src/App.jsx`, the sidebar
- Test: `frontend/src/test/products.test.jsx` (new), `admin/src/test/products.test.jsx` (new)

- [ ] **Step 1: Failing tests** (Vitest + Testing Library, mocking `api` as `account.test.jsx` does): Pricing with
  `?product=assistant` renders "Nexa Assistant" cards with `$5` and `$45`, and its checkout call carries
  `product:'assistant'`; Dashboard with `products.assistant.plan 'pro'` and `ai-usage {requests: 40, limit: 1000}`
  shows "40 of 1,000"; Activate with `productName 'Nexa Assistant'` says "Nexa Assistant … wants to sign in". Admin:
  the Releases create modal has a Product select defaulting to NDM and posts `product`; a row with `product
  'assistant'` shows the badge; the Subscriptions filter posts `product`.
- [ ] **Step 2: Run** `npm test` in both → FAIL. **Step 3: Implement.** **Step 4: Run** `npm run lint && npm test` in
  `frontend/` and `admin/` → PASS; `npm run build` in both (the prerender's canonical check still passes).
- [ ] **Step 5: Commit** `feat(website): the second product on the site and in the admin panel`.

---

### Task 9: The assistant's site

**Files:**
- Create `ndm-website/assistant/`: `package.json` (deps: react, react-dom, react-router-dom; dev: vite,
  @vitejs/plugin-react, @tailwindcss/vite, vitest, jsdom, @testing-library/{react,jest-dom,user-event},
  @playwright/test, eslint as the frontend's), `vite.config.js` (alias `@ndm` → `../frontend/src`,
  `server.fs.allow: ['..']`, the `/api` dev proxy), `vitest.config.js` (as the frontend's), `playwright.config.js`
  (as the frontend's, port 5174), `eslint.config.js`, `index.html`, `src/{main.jsx,App.jsx,index.css}`
  (`@import "tailwindcss"; @source "../../frontend/src/components"; @theme {…the frontend's tokens…}`),
  `src/api.js` (`API_BASE = import.meta.env.VITE_API_URL || '/api'`, `readPublic(path)`),
  `src/hooks/usePageMeta.js` (SITE_NAME `Nexa Assistant`, SITE_URL `https://assistant.nexadownloadmanager.com`),
  `src/components/{Header,Footer}.jsx` (Sign in → `https://nexadownloadmanager.com/login`, Dashboard →
  `/dashboard`, Privacy/Terms → the main site's pages), `src/pages/{Home,Features,Download,Pricing,Docs,NotFound}.jsx`,
  `scripts/prerender.mjs` (the frontend's, with this site's ROUTES), `scripts/inline-script-hashes.mjs` (copied),
  `src/test/{setup.js,download.test.jsx,pricing.test.jsx}`, `e2e/site.spec.js`, `public/{robots.txt,favicon.svg}`.
- Modify: `.github/workflows/website.yml` (an `assistant` job = the frontend's job on `ndm-website/assistant`).

*Deviation:* the spec moves shared components into `ndm-website/shared/`; this plan imports them from the main site
through the `@ndm` alias instead, so no import in the main site changes (a later move is mechanical). Only
presentational components are shared (`Section`, `Card`, `Button`, `Skeleton`, `Spinner`, `ErrorBoundary`,
`ScrollRegion`, `WarpField`, `CountUp`, `Brand`); the header, footer and pages are the assistant's own.

**Content decisions:** Home — hero "Your Linux PC, by voice", three pillars (Talks and listens offline · Works your PC,
your phone and the browser · Your own AI programs and keys, or Nexa AI), a Mission Control picture
(`public/mission-control.png`, taken with `--design mission --screenshot`), the download button. Features — the
seven M-features in the README's words (voice, Mission Control, screen as text, phone, web remote, providers, memory
and skills). Download — reads `/releases/latest?product=assistant`: a Linux card (version, size, SHA-256, the link
`${API_BASE}/releases/download/linux?product=assistant`, the install lines in a code block), a Windows card marked
"Coming", the "Speech models download on first start" note, requirements (Ubuntu 24.04 / Debian 13 or newer, GNOME).
Pricing — reads `/subscription/plans?product=assistant`: three cards, the buttons link to
`https://nexadownloadmanager.com/pricing?product=assistant` (paid) and `/register` (free). Docs — the README's user
parts as sections (install, first start, voice, the phone, the web remote, providers, the root helper, uninstall).

- [ ] **Step 1: Failing tests**: `download.test.jsx` — with `readPublic` mocked to `{version:'1.0.0', linuxSize:
  75497472, linuxSha256:'ab…', hasLinuxFile:true, hasWindowsFile:false}` the page shows `v1.0.0`, `72 MB`, a link
  whose `href` ends with `/releases/download/linux?product=assistant`, and "Coming" on Windows; with
  `hasLinuxFile:false` it says the build is on its way. `pricing.test.jsx` — three cards, `$5`, `$45`, `$15`,
  `$135`, "5 seats", the Pro button's `href` is the main site's pricing with `?product=assistant`.
- [ ] **Step 2: Run** `npm test` → FAIL. **Step 3: Build the app.** **Step 4: Run** `npm run lint && npm test && npm run
  build` → PASS; `npx playwright install chrome` once, `npm run test:e2e` (nav to each page, the download link's
  href) → PASS; look at `vite preview` pages with a headless Chrome screenshot of `/`, `/download`, `/pricing`.
- [ ] **Step 5: Commit** `feat(website): the assistant's site — ndm-website/assistant`.

---

### Task 10: Deploying the assistant — `.htaccess`, the proxy copy, the script, the guard, the docs

**Files:**
- Create: `ndm-website/deploy/hostinger/assistant.htaccess` (the main `.htaccess` minus the `admin`/`root` rules and
  the account-route fallback list; its own CSP `script-src` hashes; `SetEnvIf Origin` for
  `https://nexadownloadmanager.com` so the main site may call `/api` here if ever needed)
- Modify: `deploy/build-and-upload.sh` — variables `ASSISTANT="${SITE}/assistant"`, `ASSISTANT_SITE="domains/assistant.nexadownloadmanager.com"`,
  `ASSISTANT_WEBROOT="${ASSISTANT_SITE}/public_html"`, `SKIP_ASSISTANT`; **Phase 1c** builds it (`npm ci`, `npm run
  build`, the dist/canonical/baseURL checks as Phase 1, CSP hash check against `assistant.htaccess`); **Phase 4**
  requires the remote directory to exist (`test -d` over ssh, else `die "create the subdomain first (Task 11)"`);
  **Phase 5b** uploads `dist/` with `--delete-after` excluding `.htaccess`, `api-proxy.php`, `.well-known/`, then
  uploads `assistant.htaccess` as `.htaccess` (previous copy backed up to `~/${ASSISTANT_SITE}/htaccess-backups/`) and a
  generated proxy: `sed -e "s#'/../run/api.sock'#'/../../nexadownloadmanager.com/run/api.sock'#" -e
  "s#DEFAULT_HOST *= 'nexadownloadmanager.com'#DEFAULT_HOST = 'assistant.nexadownloadmanager.com'#"
  ndm-website/deploy/hostinger/api-proxy.php` (the two substitutions asserted to have matched); `guard_record
  assistant`. `deploy/deploy-guard.sh` — component `assistant` = `sha256sum '${ASSISTANT_WEBROOT}/index.html'`.
- Modify: `ndm-website/deploy/README.md` (the assistant component, the subdomain, the `.env` keys), `CONTRACT.md`
  (§5 models + `ai_usage`, §6 the `product` parameter on every route touched, `/api/assistant/v1` and its envelope
  exception in §2, §9 `PLAN_CATALOGS`), `AGENTS.md` (one paragraph: two products), `backend/.env.example` (the
  `ASSISTANT_AI_*` keys with comments), `backend/src/config/env.js` default `CORS_ORIGINS` gains
  `https://assistant.nexadownloadmanager.com`.

- [ ] **Step 1:** `bash -n deploy/build-and-upload.sh deploy/deploy-guard.sh`; a dry run of the generation:
  the `sed` on a copy of `api-proxy.php` yields both new constants (`grep -c` = 2).
- [ ] **Step 2:** `SKIP_FRONTEND=1 SKIP_ADMIN=1 SKIP_BACKEND=1 DRY_RUN=1 deploy/build-and-upload.sh` if the script has
  a dry-run switch; otherwise run only Phase 1c by sourcing the build part — the plan accepts "build passes, upload
  is Task 11".
- [ ] **Step 3:** `npm test` (backend) once more; `git diff --stat` reviewed: no file outside the map.
- [ ] **Step 4: Commit** `feat(deploy): the assistant site as a deploy component under the guard; contract and docs`.
- [ ] **Step 5:** Push the branch and open the PR `website/two-products → main` (title "Two products: the assistant's
  releases, plans, licences, the Nexa AI relay and assistant.nexadownloadmanager.com"), CI green
  (`website.yml`: backend on MariaDB, frontend, admin, assistant).

---

### Task 11: Go live (each step with the owner's go)

- [x] **Step 1 — the subdomain:** Verified `assistant.nexadownloadmanager.com` already exists on Hostinger under `~/domains/nexadownloadmanager.com/public_html/assistant` (DNS + HTTPS active). PR #86 (`f2f404e`) adapted `deploy/build-and-upload.sh` (`ASSISTANT_SITE` default `${REMOTE_SITE}`, `ASSISTANT_WEBROOT` default `${WEBROOT}/assistant`, `relative_socket_ref()`, `--exclude='assistant/'`), `public_html.htaccess` (`/assistant` 404 on main host + `assistant.` CORS + merged `Vary: Origin`), and `assistant.htaccess` so nested docroot works seamlessly.
- [ ] **Step 2 — the live `.env`:** **Partial (2026-11-16):** Appended `https://assistant.nexadownloadmanager.com` to `CORS_ORIGINS` (backed up `.env` to `.env.bak-20261116-before-m30`, kept `ADMIN_ALLOWED_IPS=*` per owner choice to handle browser checks). **`ASSISTANT_AI_UPSTREAM_URL`, `ASSISTANT_AI_UPSTREAM_KEY`, `ASSISTANT_AI_MODELS` (`alias:upstream` format) deferred by owner** (`Configure ASSISTANT_AI_* later`).
- [x] **Step 3 — deploy:** Recovered `nexadownloadmanager.com` from Hostinger `hbuilds` 503 (`01a1ded2` / `01a1e135`), merged PR #86 (`f2f404e`), and deployed cleanly via `DEPLOY_HTACCESS=1 DEPLOY_ADOPT=1 ./deploy/build-and-upload.sh`. Verified: `nexadownloadmanager.com/` 200 (`Nexa Download Manager`), `/pricing` 200, `/admin` & `/root` 200 (`X-Robots-Tag: noindex, nofollow`), `/assistant` on main host 404, `/api/releases/latest` 200 (`0.3.0`); `assistant.nexadownloadmanager.com/` 200 (`Nexa Assistant`), `/download` 200, `/pricing` 200, `/api/health` 200 (`status: up`, `billing: disabled`), `/api/releases/latest?product=assistant` 404 (`NO_RELEASE`), `/api/assistant/v1/models` 401 JSON (`UNAUTHORIZED`); CORS `Access-Control-Allow-Origin` + single `Vary: Origin` verified.
- [ ] **Step 4 — the first release:** **Deferred by owner (2026-11-16) for browser upload.** CI artifact `nexa-assistant_1.0.0_amd64.deb` (`72M`, SHA-256 `412a023747ada5807d79f29f30be86301b4d67c9484eba11b610e8f39e448607`) staged locally at `/tmp/m30-deb/nexa-assistant_1.0.0_amd64.deb`.
- [ ] **Step 5 — the relay, live:** **Deferred by owner (2026-11-16)** alongside `ASSISTANT_AI_*` and browser release/activation.
- [x] **Step 6:** Recorded the outcome, nested docroot/socket behavior, `ASSISTANT_AI_MODELS` colon delimiter fix, and the hPanel `hbuilds` disconnect warning in this plan's checklist, `ndm-website/deploy/README.md`, and the SDD ledger.

---

### Task 12: This repo's record

- [ ] `ARCHITECTURE.md`: the M30 row (what the server does for the assistant, the relay's contract the app will
  speak in M29, the subdomain), Status "M30 built"; `CLAUDE.md`: a paragraph "The server" (where the NDM repo is,
  the product parameter, `/api/assistant/v1`, the deploy command and that the live steps need the owner).
- [ ] Commit on `m20-hardcore`: `M30: the server with two products and the assistant's site — documented`.
