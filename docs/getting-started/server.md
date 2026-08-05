# Secure remote API (Ubuntu 24)

Run Arbiter as an HTTP+SSE API on a dedicated VPS so a test app can call it
over the public internet. Arbiter itself speaks **plain HTTP on loopback**;
TLS, the public socket, and edge filtering live in a reverse proxy.

This walkthrough targets **Ubuntu 24.04 LTS** (Linode or similar). The same
layout works on other systemd hosts.

Reference templates live under [`deploy/`](../../deploy/).

## Architecture

```
Client (HTTPS + Bearer atr_…)
        │
        ▼
   :443 Caddy / nginx   ← Let's Encrypt TLS, SSE timeouts, /v1/metrics deny
        │
        ▼
 127.0.0.1:8080 arbiter --api   ← systemd, user `arbiter`, HOME=/var/lib/arbiter
```

Do **not** bind Arbiter to `0.0.0.0` or open port `8080` in the firewall.
The default bind (`127.0.0.1`) is intentional — see
[`cli/api.md`](../cli/api.md) and [`SECURITY.md`](../../SECURITY.md).

## 1. Server prep

SSH in as a sudo-capable user. Create a dedicated account and data dir:

```bash
sudo adduser --system --group --home /var/lib/arbiter --shell /usr/sbin/nologin arbiter
sudo mkdir -p /var/lib/arbiter /etc/arbiter
sudo chown arbiter:arbiter /var/lib/arbiter
sudo chmod 750 /var/lib/arbiter
```

Install the binary (Linux x86_64):

```bash
curl -fsSL https://arbiter.run/install.sh | sh
# or: curl -fsSL https://arbiter.run/install.sh | ARBITER_VERSION=v0.10.0 sh
which arbiter   # expect /usr/local/bin/arbiter
```

Point DNS `A`/`AAAA` for your hostname (e.g. `arbiter.example.com`) at the
Linode. Wait until it resolves before requesting certificates.

## 2. Firewall

Only SSH and the proxy ports should be public:

```bash
sudo apt-get update
sudo apt-get install -y ufw
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow OpenSSH
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw enable
sudo ufw status
```

In the Linode Cloud Firewall (if you use one), mirror the same allow list.
Do **not** allow `8080`.

## 3. Config and secrets

```bash
sudo cp deploy/env/arbiter.env.example /etc/arbiter/arbiter.env
sudo chmod 600 /etc/arbiter/arbiter.env
sudo chown root:root /etc/arbiter/arbiter.env
sudo nano /etc/arbiter/arbiter.env
```

Set at least:

| Variable | Value |
|----------|--------|
| `OPENROUTER_API_KEY` | Your provider key |
| `ARBITER_PUBLIC_BASE_URL` | `https://arbiter.example.com` (no trailing slash) |

Optional but recommended for a public test API:

```bash
ARBITER_TENANT_MAX_CONCURRENT=4
ARBITER_TENANT_RATE_PER_MIN=30
ARBITER_TENANT_RATE_BURST=10
ARBITER_LOG_FORMAT=json
```

Leave `ARBITER_SANDBOX_IMAGE` unset until you intentionally enable sandboxed
`/exec`. Leave `ARBITER_ALLOW_HOST_EXEC` unset forever on a network-facing
host.

## 4. Seed agents and a tenant

Run once as the `arbiter` user so files land under `/var/lib/arbiter/.arbiter/`:

```bash
sudo -u arbiter -H HOME=/var/lib/arbiter arbiter --init
sudo -u arbiter -H HOME=/var/lib/arbiter arbiter --add-tenant app
```

Save the printed `atr_…` token somewhere secret — it is shown once. That is
the Bearer token your test app will use.

The first `arbiter --api` start also creates an `adm_…` admin token at
`/var/lib/arbiter/.arbiter/admin_token` (mode `0600`) unless you set
`ARBITER_ADMIN_TOKEN` in the env file. Prefer the env var for production so
the token is not only on disk under HOME.

## 5. systemd

```bash
sudo cp deploy/systemd/arbiter.service /etc/systemd/system/arbiter.service
sudo systemctl daemon-reload
sudo systemctl enable --now arbiter
sudo systemctl status arbiter
curl -sS http://127.0.0.1:8080/v1/health
# → {"ok":true}
```

Logs:

```bash
journalctl -u arbiter -f
```

## 6. TLS reverse proxy

### Option A — Caddy (recommended)

Automatic HTTPS with Let's Encrypt:

```bash
sudo apt-get install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt-get update
sudo apt-get install -y caddy

sudo cp deploy/caddy/Caddyfile /etc/caddy/Caddyfile
# edit hostname
sudo nano /etc/caddy/Caddyfile
sudo systemctl enable --now caddy
sudo systemctl reload caddy
```

### Option B — nginx + Certbot

```bash
sudo apt-get install -y nginx certbot python3-certbot-nginx
sudo cp deploy/nginx/arbiter.conf /etc/nginx/sites-available/arbiter
sudo ln -sf /etc/nginx/sites-available/arbiter /etc/nginx/sites-enabled/arbiter
sudo rm -f /etc/nginx/sites-enabled/default
# Temporarily comment out the SSL server block (or use certbot --nginx
# against a minimal HTTP vhost), then:
sudo nginx -t && sudo systemctl reload nginx
sudo certbot --nginx -d arbiter.example.com
```

Both templates disable proxy buffering and raise read timeouts so SSE streams
(`/v1/orchestrate`, conversation messages, notifications) stay open through
long LLM calls. They also return `403` for `/v1/metrics`, which is
unauthenticated by design.

## 7. Verify from your laptop

```bash
export ARBITER_URL=https://arbiter.example.com
export ARBITER_TOKEN='atr_…'   # from --add-tenant

curl -sS "$ARBITER_URL/v1/health"

curl -N -X POST "$ARBITER_URL/v1/orchestrate" \
  -H "Authorization: Bearer $ARBITER_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"agent":"index","message":"Say hello in one short sentence."}'
```

You should see SSE frames ending in a `done` event. Point your test app at
`$ARBITER_URL` with the same Bearer header on every `/v1/*` call (except
`/v1/health`).

Admin routes (`/v1/admin/*`) need the `adm_…` token, not the tenant token —
see [Authentication](../concepts/authentication.md).

## Hardening checklist

- [ ] Arbiter bound to `127.0.0.1` only; UFW/Linode firewall deny `8080`
- [ ] TLS at the proxy; `ARBITER_PUBLIC_BASE_URL` set to the `https://` origin
- [ ] Process runs as user `arbiter`, not root
- [ ] `/etc/arbiter/arbiter.env` mode `600`; tenant/admin tokens never in git
- [ ] `/exec` stays disabled (no `ARBITER_ALLOW_HOST_EXEC`, no sandbox until needed)
- [ ] Per-tenant rate / concurrency limits set for a public test API
- [ ] `/v1/metrics` blocked at the proxy
- [ ] Back up `/var/lib/arbiter/.arbiter/tenants.db` (stop the service for a clean copy, or use SQLite backup)

## Optional next steps

- **Sandboxed `/exec`:** [Sandbox](../concepts/sandbox.md) — requires Docker and
  loosening some systemd hardening so the service can talk to the docker socket.
- **Search / browse:** `arbiter --setup-tools` as the `arbiter` user, or set
  `ARBITER_SEARCH_*` in the env file — [environment](../cli/environment.md).
- **CORS for a browser SPA:** Arbiter defaults to permissive CORS for Bearer
  auth. Restrict `Access-Control-Allow-Origin` at the proxy if you need a tight
  allowlist — [Operations → CORS](../concepts/operations.md#cors).

## See also

- [`deploy/`](../../deploy/) — copy-paste unit, Caddyfile, nginx site, env example
- [`cli/api.md`](../cli/api.md) — flags and startup behaviour
- [`concepts/operations.md`](../concepts/operations.md) — proxies, limits, drain
- [`concepts/authentication.md`](../concepts/authentication.md) — `atr_` / `adm_` tokens
- [`api/index.md`](../api/index.md) — endpoint catalogue
