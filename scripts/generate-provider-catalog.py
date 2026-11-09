#!/usr/bin/env python3
"""Generates core/ai/ProviderCatalogData.cc, the catalogue behind Settings -> Providers -> Add.

    python3 -I scripts/generate-provider-catalog.py [--omniroute DIR] [--out FILE] [--check] [--keep-scratch]

This is a dev-time tool: the app never runs Node and never reads OmniRoute. The checked-in data file is its output,
and it is deterministic (no timestamps), so `--check` can tell whether the file is current.

Source. OmniRoute (MIT, Copyright (c) 2026 diegosouzapw; see THIRD_PARTY_NOTICES.md), the npm package `omniroute`.
Its provider registry (open-sse/config/providers/registry/*/index.ts, one entry per id) is TypeScript built from
helpers and spreads, so it is bundled with the package's own esbuild and read by Node into a scratch folder under
/tmp (never into the repo), together with the metadata records of src/shared/constants/providers/{apikey/*,noauth,
local}.ts (name, website, free-tier flag, notes) joined by id:

    O=~/.npm-global/lib/node_modules/omniroute
    $O/node_modules/.bin/esbuild $O/open-sse/config/providers/index.ts --bundle --platform=node --format=esm \\
        --alias:@=$O/src --alias:@omniroute/open-sse=$O/open-sse --outfile=<scratch>/reg-bundle.mjs

Nothing is fetched from the network, nothing signs in, and no header value or anonymous key is ever copied: the
dump holds header NAMES only.

What Nexa can drive. Nexa has three wire formats (core/ai/OpenAiProvider, AnthropicProvider, GeminiProvider). An
entry is kept only when a request from Nexa would be the request OmniRoute's default executor sends:

  * format openai / claude / gemini; executor "default" or one of five others judged equivalent by reading their
    files (xai, moonshot (the ids moonshot and kimi), cheaperinference, pollinations, and the label "openai-compatible"
    that dahl carries): they change the body or route a few models to /responses, never the URL or the auth of a
    chat-completions call;
  * authType apikey / optional / none; not oauth, not a web-session or cookie scraper, not a local-CLI bridge;
  * not deprecated, not media-only (no "llm" in serviceKinds, or in OmniRoute's image-only / embedding sets);
  * a fixed URL: no {template}, no per-account host, a path that ends in /chat/completions (openai), /messages
    (claude) or /models (gemini), apart from five that already hold a base (BASE_AS_GIVEN);
  * the key travels the way Nexa sends it: Authorization: Bearer (openai), x-api-key (+ Bearer, anthropic-version:
    claude), x-goog-api-key (gemini). x-api-key on the openai wire, Key/Api-Key/cookie schemes, a second key header
    (reka) and impersonating a vendor's own client (agentrouter) are left out;
  * ids that default.ts gives its own URL or header code (SPECIAL) were each read and judged; one that is not in the
    table stops the script, so a new OmniRoute cannot slip a special case in unseen.

URL rules. Query and fragment are dropped, a trailing '/' too, then the endpoint suffix is stripped (Nexa appends
/chat/completions or /messages itself; the Gemini base is the part before /models). The models list is
{base}/models (openai) or {base}/models?limit=1000 (claude) unless MODELS_URL says otherwise: it holds the entries
whose list lives elsewhere (Perplexity's under /v1 although its chat is not), from OmniRoute's own model-listing
configuration. A registry `modelsUrl` is not copied: OpenAiProvider::parseModels reads {"data":[{"id"}]} or a bare
array, and the two that differ answer in another shape (Ollama's /api/tags, Fireworks' native listing), so they stay
on the default (MODELS_URL_DEFAULT_KEPT); any other that differs is reported as a WARNING to be looked at.

Entry. id, name, kind (the Config.h kind strings), baseUrl, modelsUrl (empty = the default), requiresKey (false for
authType optional/none and for local servers), category, website (OmniRoute's referral parameters removed), hint
(a short note: only text that is true for Nexa, never anything that names OmniRoute or its dashboard, and never
anything that looks like a key), freeTier. Categories, in display order: Gateways, Model makers, Inference hosts,
Regional, Cloud platforms, Local, Free. The first five follow OmniRoute's own groups (frontier labs = Model makers,
enterprise cloud = Cloud platforms, plus its aggregator and enterprise sets); Local = a server on this computer;
Free = usable without a key (the key is optional, or there is none). A provider with a free tier that needs a key
stays in its own group and carries the free-tier flag. No model lists: the live /models is the source.

Local runtimes. OmniRoute's local.ts also describes Ollama, LM Studio, vLLM, llama.cpp, Lemonade, Llamafile, Triton,
Docker Model Runner, Xinference and oobabooga, which are not registry ids (its default executor treats them as plain
OpenAI-compatible at `localDefault`). They are added as Local entries; Ollama and LM Studio get Nexa's own preset ids
(ollama, lmstudio) so the hand-tuned preset wins on the id clash. Local entries may share a default port
(vLLM/Triton, llama.cpp/Llamafile); every other base URL appears once (PREFERRED picks among reseller aliases).

Judged by hand (reviewed after the structural rules): EXCLUDED_BY_REVIEW drops an id whose chat endpoint is not one
Nexa's wires can talk to (Coze), WEBSITE_OVERRIDES and HINT_OVERRIDES correct a note that names another platform's
site or a control Nexa does not have (Z.AI, the Alibaba and Volcano Engine plans), LOCAL_BASE_OVERRIDE a local
runtime's documented base (Docker Model Runner is at /engines/v1).

The report (stdout) gives the counts and every excluded id with its reason.
"""

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit

KIND_OPENAI = "openai-compatible"
KIND_ANTHROPIC = "anthropic"
KIND_GEMINI = "gemini"
WIRE_KIND = {"openai": KIND_OPENAI, "claude": KIND_ANTHROPIC, "gemini": KIND_GEMINI}

CATEGORIES = ["Gateways", "Model makers", "Inference hosts", "Regional", "Cloud platforms", "Local", "Free"]
GROUP_CATEGORY = {
    "apikey/gateways": "Gateways",
    "apikey/frontier-labs": "Model makers",
    "apikey/inference-hosts": "Inference hosts",
    "apikey/enterprise-cloud": "Cloud platforms",
    "apikey/regional": "Regional",
    "apikey/specialty-media": "Model makers",
    "noauth": "Gateways",
    "local": "Local",
}

# Executors whose chat-completions call is the default executor's (read in open-sse/executors/*.ts, 3.8.51).
EQUIVALENT_EXECUTORS = {"default", "xai", "moonshot", "cheaperinference", "pollinations", "openai-compatible"}

# Entries whose registry base is already the base Nexa wants (OmniRoute's own code appends the path).
BASE_AS_GIVEN = {
    "xiaomi-mimo": "normalizeXiaomiMimoChatUrl appends /chat/completions to the /v1 base",
    "xiaomi-mimo-token-plan": "same, regional cluster root",
    "mlx-gemma": "mlx_lm.server, localhost:11435/v1",
    "mlx-qwen": "mlx_lm.server, localhost:11436/v1",
    "bailian-coding-plan": "base ends in /apps/anthropic/v1 and its chatPath is /messages",
}

# Judgments for every id that default.ts gives URL or header code of its own and that passes the structural rules.
# ("keep"|"exclude", why). The URL half was checked against buildUrl, the header half against buildHeaders.
SPECIAL = {
    "anthropic": ("keep", "x-api-key only for api.anthropic.com, as Nexa sends; ?beta=true is optional"),
    "openai": ("keep", "own URL code only reroutes responses-only models; chat completions are the base URL"),
    "gemini": ("keep", "x-goog-api-key; URL is {base}/{model}:streamGenerateContent?alt=sse like GeminiProvider"),
    "xiaomi-mimo": ("keep", "bearer; base + /chat/completions"),
    "xiaomi-mimo-token-plan": ("keep", "bearer; default regional cluster base + /chat/completions"),
    "kimi-coding-apikey": ("keep", "x-api-key (Nexa adds Bearer); ?beta=true dropped (inferred optional)"),
    "bailian-coding-plan": ("keep", "x-api-key; messages URL = base + /messages"),
    "zai": ("keep", "x-api-key; claude wire at .../anthropic/v1 (the openai override is per-model, not default)"),
    "alibaba": ("keep", "bearer; normalizeOpenAIChatUrl leaves a /chat/completions base as it is"),
    "alibaba-cn": ("keep", "same as alibaba"),
    "qwen-cloud": ("keep", "same as alibaba"),
    "qwen-cloud-token-plan": ("keep", "same as alibaba"),
    "siliconflow": ("keep", "bearer; normalizeOpenAIChatUrl leaves a /chat/completions base as it is"),
    "poe": ("keep", "bearer; chat completions unless a model is Responses-only"),
    "heroku": ("keep", "bearer; US inference base + /chat/completions"),
    "agentrouter": ("exclude", "its WAF accepts only a vendor client's wire image (impersonation)"),
    "reka": ("exclude", "sends X-Api-Key beside the bearer token; Nexa cannot"),
    "gigachat": ("exclude", "needs an OAuth access token minted from client credentials"),
    "maritalk": ("exclude", "Authorization: Key <token>, not Bearer"),
    "clarifai": ("exclude", "Authorization: Key <token>, not Bearer"),
    "snowflake": ("exclude", "per-account host and a token-type header"),
    "databricks": ("exclude", "per-workspace host"),
    "datarobot": ("exclude", "per-deployment host"),
    "azure-ai": ("exclude", "api-key header and per-resource host"),
    "oci": ("exclude", "per-tenancy host"),
    "sap": ("exclude", "needs an AI-Resource-Group header"),
    "watsonx": ("exclude", "per-region host and IAM token"),
    "cline": ("exclude", "workos: bearer prefix and client headers"),
    "clinepass": ("exclude", "OAuth or BYOK with client headers"),
    "modal": ("exclude", "the endpoint is the user's own deployment (https://<workspace>--<app>.modal.run/v1)"),
}

# Headers a registry entry may carry that Nexa does not need to send (attribution, or sent by Nexa already).
HARMLESS_HEADERS = {"HTTP-Referer", "X-Title", "Anthropic-Version", "Anthropic-Beta", "Content-Type", "Accept"}
# A pinned User-Agent is fine only where the requirement is "any explicit one": Qt sends "Mozilla/5.0" (checked).
USER_AGENT_OK = {"navy": "rejects requests with no User-Agent; Qt sends Mozilla/5.0"}
USER_AGENT_BAD = {"routeway": "Cloudflare 1010 wants a browser-style User-Agent; not verifiable offline"}

# Reseller aliases that share one base URL: the one that is kept.
PREFERRED = ["moonshot", "iflytek", "volcengine", "alibaba", "qianfan", "naga-ac"]

# Where the model list is when it is not {base}/models and Nexa can read the answer (read from OmniRoute's own
# PROVIDER_MODELS_CONFIG, the part of its dashboard that lists models; verified shape {"data":[{"id"}]}).
MODELS_URL = {
    "aimlapi": "https://api.aimlapi.com/models",
    "perplexity": "https://api.perplexity.ai/v1/models",
    "deepseek": "https://api.deepseek.com/v1/models",
}

# Registry `modelsUrl`s that differ from {base}/models and were judged: they are left on the default.
MODELS_URL_DEFAULT_KEPT = {
    "ollama-cloud": "answers Ollama's own {models:[{name}]} shape, which Nexa cannot read; /v1/models is OpenAI-shaped",
    "fireworks": "Fireworks' native listing ({models:[{name}]}); /inference/v1/models is OpenAI-shaped",
    "volcengine-coding-plan": "a relative '/models', which is the default",
}

# OmniRoute's own ids for the local runtimes, mapped to Nexa's hand-tuned preset ids where they exist.
LOCAL_ID_ALIAS = {"ollama-local": "ollama", "lm-studio": "lmstudio"}

HINT_OVERRIDES = {
    "aihorde": "Volunteer-run GPU workers, so replies can take minutes. Needs a free key from aihorde.net.",
    "uncloseai": "Free. If a request is refused, enter any text as the key.",
    "pollinations": "Anonymous access is best-effort; premium models need a key from enter.pollinations.ai.",
    # OmniRoute's notes for these tell the reader to pick a region or connect an account in ITS dashboard; in Nexa
    # each region is an entry of its own, so the note says which one this is.
    "alibaba": "International (Singapore) endpoint: needs a Model Studio key from that region.",
    "alibaba-cn": "Mainland China (Beijing) endpoint: needs a Model Studio key from that region.",
    "bailian-coding-plan": "Singapore endpoint: needs an Alibaba Cloud Token Plan key.",
    "qwen-cloud-token-plan": "Singapore endpoint: needs a Qwen Cloud Token Plan key.",
    "volcengine-agent-plan": "Needs an Ark Agent Plan subscription API key.",
    "volcengine-coding-plan": "Needs an Ark Coding Plan subscription API key.",
    # api.z.ai is the international platform; OmniRoute's note and website point at the separate China one.
    "zai": "International platform (api.z.ai): create the key at z.ai.",
    "g4f-groq": "Free third-party gateway: g4f.space handles your prompts.",
    "g4f-gemini": "Free third-party gateway: g4f.space handles your prompts.",
    "g4f-pollinations": "Free third-party gateway: g4f.space handles your prompts.",
    "g4f-ollama": "Free third-party gateway: g4f.space handles your prompts.",
    "g4f-nvidia": "Free third-party gateway: g4f.space handles your prompts.",
}

# Websites that OmniRoute gives for another platform than the one the base URL belongs to.
WEBSITE_OVERRIDES = {
    "zai": "https://z.ai",
}

# Judged by hand after the structural rules passed: not an endpoint Nexa's wires can talk to.
EXCLUDED_BY_REVIEW = {
    "coze": "agent platform: chat is /v3/chat with a bot id, not an OpenAI-compatible /v1/chat/completions",
}

# Default base URLs of local runtimes where OmniRoute's `localDefault` is not the runtime's documented OpenAI base.
LOCAL_BASE_OVERRIDE = {
    "docker-model-runner": "http://localhost:12434/engines/v1",
}

HINT_MAX = 140
# Text that is about OmniRoute's own dashboard or audit notes, or that shows how a key is written, is not for Nexa's
# users: such a note is skipped and the next field tried.
SKIP_HINT = re.compile(
    r"omniroute|add account|import from|auto-?sync|dashboard|passthrough|paste (?:it|them) here|this provider|"
    r"available models|\boauth\b|sign in with|jwt session|audit|\bbearer\b|authorization:|\bsk-|<[a-z _-]+>|"
    r"\bexposes\b|openai-compatible (?:endpoint|base url)|is openai-compatible at|only after confirming",
    re.IGNORECASE,
)
PREFIX_ELLIPSIS = re.compile(r"[_-](?:\.\.\.|…)")
LOCAL_HOSTS = {"localhost", "127.0.0.1", "::1", "[::1]"}
REFERRAL_KEYS = {"aff", "ref", "referral", "via", "source"}


# ---------------------------------------------------------------------------------------------------------------
# Reading OmniRoute

REGISTRY_FIELDS_MJS = r"""
// Dumps the registry and the metadata as plain JSON. Header VALUES and anonymous keys are never written.
import { writeFileSync } from "node:fs";
const reg = await import("./reg-bundle.mjs");
const meta = await import("./meta-bundle.mjs");
const scalar = (v) => (typeof v === "string" || typeof v === "number" || typeof v === "boolean" ? v : null);
const registry = [];
for (const [key, e] of Object.entries(reg.REGISTRY)) {
  const models = Array.isArray(e.models) ? e.models : [];
  registry.push({
    key, id: e.id, format: scalar(e.format), executor: scalar(e.executor),
    baseUrl: scalar(e.baseUrl), authType: scalar(e.authType), authHeader: scalar(e.authHeader),
    authPrefix: scalar(e.authPrefix), modelsUrl: scalar(e.modelsUrl),
    headerNames: e.headers && typeof e.headers === "object" ? Object.keys(e.headers) : [],
    extraHeaderNames: e.extraHeaders && typeof e.extraHeaders === "object" ? Object.keys(e.extraHeaders) : [],
    hasAnonymousApiKey: !!e.anonymousApiKey, modelCount: models.length,
  });
}
const records = [];
for (const [group, obj] of Object.entries(meta.GROUPS)) {
  for (const [key, p] of Object.entries(obj)) {
    records.push({
      group, key, id: p.id, name: scalar(p.name), website: scalar(p.website), authHint: scalar(p.authHint),
      apiHint: scalar(p.apiHint), freeNote: scalar(p.freeNote), hasFree: !!p.hasFree,
      serviceKinds: Array.isArray(p.serviceKinds) ? p.serviceKinds : null, deprecated: !!p.deprecated,
      localDefault: scalar(p.localDefault),
      notice: p.notice ? (typeof p.notice === "string" ? p.notice : scalar(p.notice.text)) : null,
    });
  }
}
writeFileSync(process.argv[2], JSON.stringify({ registry, records, sets: meta.SETS }, null, 1));
process.exit(0);
"""


def meta_entry_mjs(omniroute: Path) -> str:
    base = f"{omniroute}/src/shared/constants/providers"
    return f"""
import {{ APIKEY_PROVIDERS_GATEWAYS }} from "{base}/apikey/gateways";
import {{ APIKEY_PROVIDERS_FRONTIER }} from "{base}/apikey/frontier-labs";
import {{ APIKEY_PROVIDERS_INFERENCE }} from "{base}/apikey/inference-hosts";
import {{ APIKEY_PROVIDERS_ENTERPRISE }} from "{base}/apikey/enterprise-cloud";
import {{ APIKEY_PROVIDERS_REGIONAL }} from "{base}/apikey/regional";
import {{ APIKEY_PROVIDERS_SPECIALTY }} from "{base}/apikey/specialty-media";
import {{ NOAUTH_PROVIDERS }} from "{base}/noauth";
import {{ LOCAL_PROVIDERS }} from "{base}/local";
import {{ IMAGE_ONLY_PROVIDER_IDS, AGGREGATOR_PROVIDER_IDS, ENTERPRISE_CLOUD_PROVIDER_IDS,
         EMBEDDING_RERANK_PROVIDER_IDS }} from "{omniroute}/src/shared/constants/providers";
export const GROUPS = {{
  "apikey/gateways": APIKEY_PROVIDERS_GATEWAYS, "apikey/frontier-labs": APIKEY_PROVIDERS_FRONTIER,
  "apikey/inference-hosts": APIKEY_PROVIDERS_INFERENCE, "apikey/enterprise-cloud": APIKEY_PROVIDERS_ENTERPRISE,
  "apikey/regional": APIKEY_PROVIDERS_REGIONAL, "apikey/specialty-media": APIKEY_PROVIDERS_SPECIALTY,
  noauth: NOAUTH_PROVIDERS, local: LOCAL_PROVIDERS,
}};
export const SETS = {{
  imageOnly: [...IMAGE_ONLY_PROVIDER_IDS], aggregator: [...AGGREGATOR_PROVIDER_IDS],
  enterpriseCloud: [...ENTERPRISE_CLOUD_PROVIDER_IDS], embeddingRerank: [...EMBEDDING_RERANK_PROVIDER_IDS],
}};
"""


def find_omniroute(given):
    candidates = [given] if given else []
    candidates += [os.environ.get("OMNIROUTE_DIR"), str(Path.home() / ".npm-global/lib/node_modules/omniroute")]
    try:
        root = subprocess.run(["npm", "root", "-g"], capture_output=True, text=True, timeout=30).stdout.strip()
        if root:
            candidates.append(str(Path(root) / "omniroute"))
    except (OSError, subprocess.SubprocessError):
        pass
    for candidate in candidates:
        if candidate and (Path(candidate) / "package.json").is_file():
            return Path(candidate).resolve()
    sys.exit("OmniRoute not found: pass --omniroute <dir> (the npm package folder).")


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        sys.exit(f"{command[0]} failed:\n{result.stdout}\n{result.stderr}")
    return result


def dump_omniroute(omniroute: Path, scratch: Path) -> dict:
    esbuild = omniroute / "node_modules/.bin/esbuild"
    if not esbuild.exists():
        sys.exit(f"esbuild not found at {esbuild}")
    common = ["--bundle", "--platform=node", "--format=esm", f"--alias:@={omniroute}/src",
              f"--alias:@omniroute/open-sse={omniroute}/open-sse", "--log-level=warning"]
    run([str(esbuild), f"{omniroute}/open-sse/config/providers/index.ts", *common,
         f"--outfile={scratch}/reg-bundle.mjs"], scratch)
    (scratch / "meta-entry.mjs").write_text(meta_entry_mjs(omniroute))
    run([str(esbuild), str(scratch / "meta-entry.mjs"), *common, f"--outfile={scratch}/meta-bundle.mjs"], scratch)
    (scratch / "dump.mjs").write_text(REGISTRY_FIELDS_MJS)
    run(["node", str(scratch / "dump.mjs"), str(scratch / "dump.json")], scratch)
    return json.loads((scratch / "dump.json").read_text())


def special_ids(omniroute: Path) -> set:
    """Ids that DefaultExecutor.buildUrl / buildHeaders handle in a `case "<id>":` of their own."""
    text = (omniroute / "open-sse/executors/default.ts").read_text()
    try:
        url_start, header_start = text.index("  buildUrl("), text.index("  buildHeaders(")
        header_end = text.index("Downgrade `response_format", header_start)
    except ValueError as error:
        sys.exit(f"default.ts no longer has the layout this script reads ({error}); re-read it and update special_ids().")
    found = set()
    for region in (text[url_start:header_start], text[header_start:header_end]):
        found.update(re.findall(r'case "([^"]+)":', region))
    return found


# ---------------------------------------------------------------------------------------------------------------
# Filtering

def looks_secret(text: str) -> bool:
    """The checks of MemoryStore::looksSecret, so the C++ test agrees with this script."""
    keyword = re.compile(r"\b(?:password|passwd|passphrase|pass ?code|pin(?: code| number)?|api[ _-]?key|secret(?: key)?"
                         r"|access token|auth token|token|private key|otp|cvv|cvc|security code)\s*(?:is|was|=|:)\s*\S+",
                         re.IGNORECASE)
    if keyword.search(text):
        return True
    for pattern in (r"\bsk-[A-Za-z0-9_-]{16,}", r"\bgh[pousr]_[A-Za-z0-9]{20,}", r"\bAKIA[0-9A-Z]{16}\b",
                    r"\bAIza[0-9A-Za-z_-]{30,}", r"\bxox[abprs]-[A-Za-z0-9-]{10,}",
                    r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.", r"-----BEGIN [A-Z ]*PRIVATE KEY-----"):
        if re.search(pattern, text):
            return True
    for run_ in re.findall(r"[A-Za-z0-9_+/=-]{32,}", text):
        if any(c.isalpha() for c in run_) and any(c.isdigit() for c in run_):
            return True
    return False


def endpoint_suffix(path: str):
    for suffix in ("/chat/completions", "/messages", "/responses", "/models"):
        if path.endswith(suffix):
            return suffix
    return None


def normalise_base(entry: dict):
    """(base url, None) or (None, reason)."""
    wire, url = entry["format"], entry["baseUrl"]
    parts = urlsplit(url)
    if parts.scheme not in ("http", "https") or not parts.hostname:
        return None, "url:not-http"
    path = parts.path.rstrip("/")
    suffix = endpoint_suffix(path)
    wanted = {"openai": "/chat/completions", "claude": "/messages", "gemini": "/models"}[wire]
    if suffix == wanted:
        path = path[: -len(wanted)]
    elif entry["id"] not in BASE_AS_GIVEN:
        return None, f"url:path ends {suffix or 'in neither endpoint'} (not {wanted}; proprietary or verbatim)"
    base = urlunsplit((parts.scheme, parts.netloc, path, "", ""))
    if parts.scheme == "http" and parts.hostname not in LOCAL_HOSTS:
        return None, "url:plain http to a remote host"
    return base, None


def auth_ok(entry: dict):
    wire, header, prefix = entry["format"], entry["authHeader"], entry["authPrefix"]
    if wire == "openai":
        if header == "bearer" or (header == "Authorization" and prefix == "Bearer"):
            return None
        return f"auth:{header} on the openai wire"
    if wire == "claude":
        return None if header in ("x-api-key", "bearer") else f"auth:{header} on the claude wire"
    return None if header == "x-goog-api-key" else f"auth:{header} on the gemini wire"


def classify(dump: dict, specials: set):
    registry = {e["id"]: e for e in dump["registry"]}
    records = {r["id"]: r for r in dump["records"]}
    sets = {name: set(ids) for name, ids in dump["sets"].items()}
    kept, excluded = {}, collections.defaultdict(list)

    def exclude(entry_id, reason):
        excluded[reason].append(entry_id)

    for entry_id, entry in registry.items():
        meta = records.get(entry_id)
        if entry["authType"] == "oauth":
            exclude(entry_id, "oauth: sign-in goes through the vendor's program, not a key"); continue
        if entry["format"] not in WIRE_KIND:
            exclude(entry_id, f"format:{entry['format']}"); continue
        if entry["executor"] not in EQUIVALENT_EXECUTORS:
            web = entry["authHeader"] == "cookie" or str(entry["executor"]).endswith("-web")
            exclude(entry_id, "executor: web session or cookie" if web else "executor: custom code"); continue
        if meta is None:
            exclude(entry_id, "no metadata record (no name or website)"); continue
        if entry_id in EXCLUDED_BY_REVIEW:
            exclude(entry_id, f"review: {EXCLUDED_BY_REVIEW[entry_id]}"); continue
        if meta["deprecated"]:
            exclude(entry_id, "deprecated"); continue
        kinds = meta["serviceKinds"] or []
        if "llm" not in kinds or entry_id in sets["imageOnly"] or entry_id in sets["embeddingRerank"]:
            exclude(entry_id, "media-only or embedding-only"); continue
        if not entry["baseUrl"]:
            exclude(entry_id, "url:none"); continue
        if re.search(r"[{}<>]", entry["baseUrl"]) or re.search(r"0{8,}", entry["baseUrl"]):
            exclude(entry_id, "url:templated or per-account host"); continue
        if entry_id in specials:
            verdict, why = SPECIAL.get(entry_id, (None, None))
            if verdict is None:
                sys.exit(f"{entry_id}: default.ts has code of its own for this id and SPECIAL has no judgment. "
                         "Read buildUrl/buildHeaders for it and add one.")
            if verdict == "exclude":
                exclude(entry_id, f"special: {why}"); continue
        base, reason = normalise_base(entry)
        if reason:
            exclude(entry_id, reason); continue
        reason = auth_ok(entry)
        if reason:
            exclude(entry_id, reason); continue
        headers = set(entry["headerNames"]) | set(entry["extraHeaderNames"])
        if headers & {"User-Agent"}:
            if entry_id in USER_AGENT_BAD:
                exclude(entry_id, f"header: User-Agent ({USER_AGENT_BAD[entry_id]})"); continue
            if entry_id not in USER_AGENT_OK:
                exclude(entry_id, "header: a pinned User-Agent nobody has judged"); continue
        other = sorted(headers - HARMLESS_HEADERS - {"User-Agent"})
        if other:
            exclude(entry_id, f"header: needs {', '.join(other)}"); continue
        kept[entry_id] = {"entry": entry, "meta": meta, "base": base}
    return registry, records, sets, kept, excluded


def drop_duplicates(kept: dict, excluded: dict, notes: list):
    groups = collections.defaultdict(list)
    for entry_id, item in kept.items():
        host = urlsplit(item["base"]).hostname
        if host in LOCAL_HOSTS:
            continue  # local runtimes may share a default port
        groups[(item["entry"]["format"], item["base"].lower())].append(entry_id)
    for (_wire, base), ids in sorted(groups.items()):
        if len(ids) < 2:
            continue
        winner = next((i for i in PREFERRED if i in ids), None)
        if winner is None:
            winner = sorted(ids)[0]
            notes.append(f"WARNING: no PREFERRED entry for the shared base {base}: {ids}; kept {winner}")
        for entry_id in sorted(ids):
            if entry_id != winner:
                excluded[f"duplicate base URL (kept {winner})"].append(entry_id)
                del kept[entry_id]


# ---------------------------------------------------------------------------------------------------------------
# Building the rows

def clean_website(url: str) -> str:
    parts = urlsplit(url.strip())
    keep = []
    for pair in parts.query.split("&") if parts.query else []:
        key, _, value = pair.partition("=")
        if key.lower() in REFERRAL_KEYS or key.lower().startswith("utm_") or "omniroute" in value.lower():
            continue
        keep.append(pair)
    return urlunsplit((parts.scheme, parts.netloc, parts.path, "&".join(keep), ""))


def first_sentence(text: str):
    """The first sentence of a note when it fits in HINT_MAX characters, else None (no cut-off text in a hint)."""
    text = re.sub(r"\s+", " ", text).strip()
    match = re.search(r"(?<!\be\.g)(?<!\bi\.e)[.!?](?= [A-Z])", text)
    sentence = text[: match.end()] if match else text
    sentence = sentence.rstrip(";,: ")
    return sentence if 0 < len(sentence) <= HINT_MAX else None


def make_hint(meta: dict) -> str:
    for field in ("notice", "freeNote", "authHint", "apiHint"):
        text = meta.get(field)
        if not text or SKIP_HINT.search(text) or PREFIX_ELLIPSIS.search(text):
            continue
        hint = first_sentence(text)
        if hint and not looks_secret(hint):
            return hint
    return ""


def local_hint(base: str) -> str:
    parts = urlsplit(base)
    return f"Runs on this computer (default {parts.netloc}). No key needed."


def category_of(entry_id, group, requires_key, local, sets):
    if local:
        return "Local"
    if not requires_key:
        return "Free"
    if entry_id in sets["aggregator"]:
        return "Gateways"
    if entry_id in sets["enterpriseCloud"]:
        return "Cloud platforms"
    return GROUP_CATEGORY[group]


def build_rows(sets, kept, notes):
    rows = []
    for entry_id, item in kept.items():
        entry, meta, base = item["entry"], item["meta"], item["base"]
        local = urlsplit(base).hostname in LOCAL_HOSTS
        requires_key = entry["authType"] == "apikey" and not local
        wire = entry["format"]
        default_models = base + ("/models?limit=1000" if wire == "claude" else "/models")
        models_url = MODELS_URL.get(entry_id, "")
        if models_url == default_models:
            models_url = ""
        registry_models = entry["modelsUrl"]
        if (registry_models and registry_models.split("?")[0] != base + "/models" and entry_id not in MODELS_URL
                and entry_id not in MODELS_URL_DEFAULT_KEPT):
            notes.append(f"WARNING: {entry_id}: the registry lists models at {registry_models}, not {default_models};"
                         " judge it (MODELS_URL or MODELS_URL_DEFAULT_KEPT)")
        hint = HINT_OVERRIDES.get(entry_id) or (local_hint(base) if local else make_hint(meta))
        rows.append({
            "id": entry_id, "name": re.sub(r"\s+", " ", meta["name"]).strip(), "kind": WIRE_KIND[wire],
            "baseUrl": base, "modelsUrl": models_url, "requiresKey": requires_key,
            "category": category_of(entry_id, meta["group"], requires_key, local, sets),
            "website": WEBSITE_OVERRIDES.get(entry_id) or clean_website(meta["website"] or ""), "hint": hint,
            "freeTier": bool(meta["hasFree"]) or (not requires_key and not local),
        })
    return rows


def add_local_runtimes(records, registry, rows, notes):
    """local.ts entries that are not registry ids: plain OpenAI-compatible servers at their `localDefault`."""
    added = []
    for entry_id, meta in sorted(records.items()):
        if meta["group"] != "local" or entry_id in registry:
            continue
        if "llm" not in (meta["serviceKinds"] or []) or not meta["localDefault"]:
            continue
        base = LOCAL_BASE_OVERRIDE.get(entry_id, meta["localDefault"]).rstrip("/")
        if urlsplit(base).scheme not in ("http", "https") or urlsplit(base).hostname not in LOCAL_HOSTS:
            continue
        final_id = LOCAL_ID_ALIAS.get(entry_id, entry_id)
        rows.append({
            "id": final_id, "name": re.sub(r"\s+", " ", meta["name"]).strip(), "kind": KIND_OPENAI, "baseUrl": base,
            "modelsUrl": "", "requiresKey": False, "category": "Local", "website": clean_website(meta["website"] or ""),
            "hint": local_hint(base), "freeTier": False,
        })
        added.append(final_id)
    return added


def category_rank(category):
    return CATEGORIES.index(category)


def sort_rows(rows):
    rows.sort(key=lambda r: (category_rank(r["category"]), r["name"].lower(), r["id"]))


def validate(rows):
    ids = [r["id"] for r in rows]
    assert len(ids) == len(set(ids)), "duplicate ids"
    for row in rows:
        assert re.fullmatch(r"[a-z0-9-]+", row["id"]), row["id"]
        assert row["name"], row["id"]
        assert row["kind"] in (KIND_OPENAI, KIND_ANTHROPIC, KIND_GEMINI), row["id"]
        assert row["category"] in CATEGORIES, row["id"]
        assert endpoint_suffix(urlsplit(row["baseUrl"]).path) is None or row["id"] in BASE_AS_GIVEN, row["id"]
        assert not row["baseUrl"].endswith("/"), row["id"]
        assert row["website"].startswith("https://"), (row["id"], row["website"])
        for field in ("id", "name", "baseUrl", "modelsUrl", "website", "hint"):
            assert not looks_secret(row[field]), (row["id"], field)
    shared = collections.defaultdict(list)
    for row in rows:
        shared[(row["kind"], row["baseUrl"].lower())].append(row)
    for key, group in shared.items():
        assert len(group) == 1 or all(r["category"] == "Local" for r in group), (key, [r["id"] for r in group])
    for category in CATEGORIES:
        assert any(r["category"] == category for r in rows), f"category {category} is empty"


# ---------------------------------------------------------------------------------------------------------------
# Writing the C++

def c_string(text: str) -> str:
    out = []
    for byte in text.encode("utf-8"):
        char = chr(byte)
        if char in '"\\?':
            out.append("\\" + char)
        elif 0x20 <= byte < 0x7F:
            out.append(char)
        else:
            out.append("\\%03o" % byte)
    return '"' + "".join(out) + '"'


def render(rows, version, command):
    lines = [
        "// GENERATED by scripts/generate-provider-catalog.py: do not edit by hand.",
        f"// Source: OmniRoute {version} (MIT, Copyright (c) 2026 diegosouzapw; see THIRD_PARTY_NOTICES.md), its provider",
        "// registry joined with its provider metadata. Regenerate, or check this file is current, with:",
        f"//     {command}",
        f"// {len(rows)} entries, sorted by category (Gateways, Model makers, Inference hosts, Regional, Cloud platforms,",
        "// Local, Free), then by name. Plain C strings in a constant table: nothing runs at load.",
        '#include "core/ai/ProviderCatalog.h"',
        "",
        "namespace nexa {",
        "namespace catalogdata {",
        "",
        "// id, name, kind, baseUrl, modelsUrl, requiresKey, category, website, hint, freeTier",
        "const Row kRows[] = {",
    ]
    current = None
    for row in rows:
        if row["category"] != current:
            current = row["category"]
            lines.append(f"    // {current}")
        fields = [c_string(row["id"]), c_string(row["name"]), c_string(row["kind"]), c_string(row["baseUrl"]),
                  c_string(row["modelsUrl"]), "true" if row["requiresKey"] else "false", c_string(row["category"]),
                  c_string(row["website"]), c_string(row["hint"]), "true" if row["freeTier"] else "false"]
        lines.append("    {" + ", ".join(fields) + "},")
    lines += ["};", "", f"const std::size_t kRowCount = {len(rows)};", "", "} // namespace catalogdata",
              "} // namespace nexa", ""]
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------------------------
# Report

def report(registry, records, kept_registry, rows, excluded, added_local, notes):
    print(f"OmniRoute registry: {len(registry)} ids; metadata records: {len(records)}")
    total_excluded = sum(len(v) for v in excluded.values())
    print(f"kept from the registry: {kept_registry}; excluded: {total_excluded}; "
          f"sum = {kept_registry + total_excluded} (registry {len(registry)})")
    print(f"added from local.ts (not registry ids): {len(added_local)}: {', '.join(added_local)}")
    print(f"catalogue entries written: {len(rows)}")
    print("by kind:", dict(collections.Counter(r["kind"] for r in rows)))
    by_category = collections.Counter(r["category"] for r in rows)
    print("by category:", {c: by_category[c] for c in CATEGORIES})
    print("requiresKey:", dict(collections.Counter(r["requiresKey"] for r in rows)),
          "| freeTier:", dict(collections.Counter(r["freeTier"] for r in rows)),
          "| with modelsUrl:", sum(1 for r in rows if r["modelsUrl"]), "| with hint:", sum(1 for r in rows if r["hint"]))
    print("excluded, by reason:")
    for reason in sorted(excluded, key=lambda r: (-len(excluded[r]), r)):
        ids = sorted(excluded[reason])
        print(f"  {len(ids):3d}  {reason}" + (f": {' '.join(ids)}" if len(ids) <= 40 else ""))
    for note in notes:
        print(note)


def main():
    parser = argparse.ArgumentParser(description="Generate core/ai/ProviderCatalogData.cc from OmniRoute's registry.")
    parser.add_argument("--omniroute", help="the omniroute npm package folder")
    parser.add_argument("--out", help="the file to write (default: core/ai/ProviderCatalogData.cc in this repo)")
    parser.add_argument("--check", action="store_true", help="write nothing; exit 1 if the file is not current")
    parser.add_argument("--keep-scratch", action="store_true", help="keep the scratch folder under /tmp")
    parser.add_argument("--list", action="store_true", help="also print every entry, one per line, for review")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    out = Path(args.out) if args.out else repo / "core/ai/ProviderCatalogData.cc"
    omniroute = find_omniroute(args.omniroute)
    version = json.loads((omniroute / "package.json").read_text()).get("version", "unknown")

    scratch = Path(tempfile.mkdtemp(prefix="nexa-catalog-", dir="/tmp")).resolve()
    try:
        dump = dump_omniroute(omniroute, scratch)
        specials = special_ids(omniroute)
        registry, records, sets, kept, excluded = classify(dump, specials)
        notes = []
        drop_duplicates(kept, excluded, notes)
        kept_registry = len(kept)
        rows = build_rows(sets, kept, notes)
        added_local = add_local_runtimes(records, registry, rows, notes)
        sort_rows(rows)
        validate(rows)
        command = "python3 -I scripts/generate-provider-catalog.py"
        text = render(rows, version, command)
        report(registry, records, kept_registry, rows, excluded, added_local, notes)
        if args.list:
            for row in rows:
                print("\t".join([row["category"], row["id"], row["name"], row["kind"], row["baseUrl"], row["modelsUrl"],
                                 "key" if row["requiresKey"] else "nokey", "free" if row["freeTier"] else "-",
                                 row["hint"]]))
        if args.check:
            current = out.read_text() if out.exists() else ""
            print("up to date" if current == text else f"{out} is NOT current: run the script")
            sys.exit(0 if current == text else 1)
        out.write_text(text)
        print(f"wrote {out}")
    finally:
        if args.keep_scratch:
            print(f"scratch kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
