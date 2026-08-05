# Deploy Arbiter as a network API

Templates for running `arbiter --api` on a dedicated Linux host (Ubuntu 24
LTS on Linode is the reference path) with:

- a dedicated unprivileged system user
- systemd process supervision
- TLS termination at Caddy or nginx (Arbiter speaks plain HTTP on loopback)
- UFW locking the host down to SSH + HTTPS

**Full walkthrough:** [`docs/getting-started/server.md`](../docs/getting-started/server.md)

| Path | Purpose |
|------|---------|
| [`env/arbiter.env.example`](env/arbiter.env.example) | Secrets + runtime env for systemd `EnvironmentFile=` |
| [`systemd/arbiter.service`](systemd/arbiter.service) | Unit file: loopback bind, graceful drain on stop |
| [`caddy/Caddyfile`](caddy/Caddyfile) | Automatic HTTPS reverse proxy (recommended) |
| [`nginx/arbiter.conf`](nginx/arbiter.conf) | nginx + Certbot alternative, SSE-safe |

Security posture matches [`SECURITY.md`](../SECURITY.md) hardening notes and
[`docs/concepts/operations.md`](../docs/concepts/operations.md): Arbiter stays
on `127.0.0.1:8080`; only the proxy faces the public internet.
