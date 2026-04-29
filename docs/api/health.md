# `GET /v1/health`

**Auth:** none — _Status:_ stable

Liveness probe. Returns `200 OK` with body `ok\n`. Used by load balancers, container orchestrators, and uptime monitors.

## Request

No path params, no query params, no body, no auth header.

```bash
curl http://arbiter.example.com/v1/health
```

## Response

### 200 OK

```
ok
```

`Content-Type: text/plain; charset=utf-8`. Single line, trailing newline.

## Failure modes

| Status | When |
|--------|------|
| 404    | Wrong path. |
| 5xx    | Daemon down. (No 5xx is intentionally returned by this endpoint — anything other than 200 means the process or its proxy is broken.) |

## See also

- [Operational notes](concepts/operations.md) — deployment + reverse-proxy guidance.
