# M21 — Away from home: the phone reaches Nexa from anywhere

Date: 2026-11-11 · Status: approved by the owner (choices in chat) · Branch: `m20-hardcore` (continues)

## Why

The owner: "I gave you a scenario that does not depend on where I am: make sure I can control the laptop or Nexa
through the mobile — a tunnel or a VPN." Today the phone reaches Nexa on the same Wi-Fi (HTTPS on the LAN address) or
through adb (`localhost:7777`); away from home, nothing.

Decided with the owner (2026-11-11): **all three ways** —

- **Tailscale** (WireGuard): a private network of the owner's own devices; Nexa is never on the internet; a fixed
  address (`nexa-laptop.<tailnet>.ts.net`, `100.x.y.z`). Installed on the laptop already; the phone runs the Tailscale
  app on the same account.
- **Cloudflare Tunnel** on the owner's domain: `nexa.nexacodesolutions.online` (the domain's DNS moves from Hostinger to
  Cloudflare; its website and mail records are copied); a quick tunnel (`*.trycloudflare.com`, changes every start)
  when no domain is set.
- **ngrok** with the owner's free fixed domain (`*.ngrok-free.app`); the owner sets the authtoken in ngrok's own
  config — Nexa never reads or stores it.

## How

**The PC**

- `RemoteServer` gains a third door: plain HTTP on loopback at `port + 2`, for tunnels only (cloudflared and ngrok
  connect to it; the TLS is theirs, at their edge). A session that came through it is *public*: it may pair (the
  six-digit code, single use, five minutes, ten wrong guesses in all withdraw it — every tunnel request comes from
  127.0.0.1, so the per-address limit is the global one), and it is labelled "through Cloudflare / ngrok" in Settings.
- `remote/RemoteRoutes` (pure): the ways a phone can reach this PC — adb, LAN, Tailscale, Cloudflare, ngrok — each a
  URL and whether it is *direct* (Nexa's own certificate, pinned by fingerprint) or through a tunnel (a CA certificate);
  ordered, encoded for the wire.
- The `welcome` carries `routes`, and a `routes` message follows when they change (a tunnel came up, Tailscale signed
  in), so a phone learns every way while it is connected and keeps them.
- `system/net/TailscaleStatus`: `tailscale status --json`, read only — signed in or not, the 100.x address, the
  MagicDNS name. `system/net/TunnelProcess` (Strategy): `CloudflareTunnel` (`cloudflared tunnel run --url … <name>`
  for the named tunnel, `cloudflared tunnel --url …` for a quick one — its URL read from its log) and `NgrokTunnel`
  (`ngrok http --url=<domain> …`, its URL from its JSON log); started and stopped with Settings, restarted with
  backoff, found on the PATH, `~/.local/bin` or `~/.local/share/nexa/tunnels`. The URL parsers are pure and tested.
- `app/RemoteAccess` (Mediator): the config → which tunnels run → the routes → `RemoteServer::setRoutes`.
- Config `remote.tailscale` (offer the tailnet address, default on), `remote.cloudflare` (`mode`: off | quick |
  named, `tunnel`, `hostname`), `remote.ngrok` (`enabled`, `domain`).
- Settings → Remote → **Away from home**: one row per way — its state in words (not installed, signed out, connecting,
  "reachable at …"), the switch or the fields, and what the owner has to do once.
- `scripts/download-tunnels.sh`: cloudflared and ngrok at pinned versions and checksums into
  `~/.local/share/nexa/tunnels` (no sudo).

**The phone**

- `PhoneSettings` keeps the routes learned from the PC besides the one it paired on.
- `RemoteClient` tries them in turn: the last that worked first, then adb, LAN, Tailscale, Cloudflare, ngrok. A direct
  route must show the pinned certificate; a tunnel's must be one the system trusts. Every request carries
  `ngrok-skip-browser-warning` (ngrok's free plan shows browsers a warning page otherwise).
- The pill says how: "Your PC · connected" at home, "· via Tailscale" / "· via Cloudflare" / "· via ngrok" away.

**Security**

- Tailscale is private: only the owner's signed-in devices reach the port. Cloudflare and ngrok are public: anyone can
  reach the page and `/api/pair`; everything else needs a paired phone's token, and the tunnel operator can read the
  traffic (their TLS ends at their edge) — the owner chose them knowing this. A tunnel runs only while its switch is on.
- Nexa never handles the vendors' credentials: Tailscale's key stays with `tailscaled`, cloudflared's `cert.pem` and
  tunnel credentials in `~/.cloudflared`, ngrok's authtoken in ngrok's own config.
- The laptop must be awake: away from home, a suspended laptop answers nothing (GNOME's automatic suspend).

## Tests

`RemoteRoutesTest` (order, wire), `TunnelLogTest` (cloudflared's and ngrok's URL lines, failures), `TailscaleStatusTest`
(the JSON of signed in, signed out, no daemon), `RemoteProtocolTest` (`routes`), `RemoteServerIntegrationTest` (the
tunnel door: a session through it is public, pairing through it works, the LAN door does not count as public),
`RemoteClientTest` (failover to the next route, a direct route with the wrong certificate refused, a tunnel route
accepted), `ConfigStoreTest`, `PhoneWindowTest` (the pill says the route). Live: the phone on mobile data reaching Nexa
through each of the three, once the owner's accounts are set up.
