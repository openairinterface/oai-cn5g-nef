# Troubleshooting

---

## Diagnosis Tools

Use these commands as a first step when investigating any NEF issue.

| Tool | Purpose | Command |
|---|---|---|
| Health endpoint | Check NEF liveness | `curl http://localhost:8080/health` |
| Docker logs | View NEF logs | `docker logs oai-nef 2>&1 \| tail -50` |
| Live log follow | Tail logs continuously | `docker logs -f oai-nef` |
| Grep for errors | Find error messages | `docker logs oai-nef 2>&1 \| grep -i "error\|fail"` |

> **Note:** NEF uses HTTP/2 cleartext (h2c). Append `--http2-prior-knowledge` to any `curl` command. See [HTTP/2 Connection Issues](#http2-connection-issues) if curl returns an error on this flag.

---

## Common Problems

### NEF returns 503 on API calls

**Symptom**: All API calls return `503 Service Unavailable` with a Problem Detail body.

**Cause A — Circuit breaker OPEN for a southbound NF**

NEF's circuit breaker trips to OPEN after 5 consecutive failures to a southbound NF (AMF, SMF, PCF, or UDR). While OPEN, all requests that require that NF are rejected immediately without a network call.

**Fix**:
1. Check logs for `[circuit_breaker] NF_TYPE OPEN` messages: `docker logs oai-nef 2>&1 | grep circuit_breaker`
2. Verify the target NF (AMF/PCF/UDR) is running and reachable from inside the NEF container.
3. Wait for `recovery_timeout` (default 30 s) for the circuit to transition to HALF-OPEN and send a probe. If the probe succeeds the circuit returns to CLOSED automatically.
4. Alternatively, restart the unavailable NF to force a fresh connection once NEF probes.

See [Resilience Guide](resilience.md#circuit-breaker) for the full state machine and configuration parameters.

**Cause B — NRF registration failed; NEF could not discover southbound NFs**

If NEF cannot register with NRF at startup it has no NF endpoints to call.

**Fix**:
1. `docker logs oai-nef 2>&1 | grep NRF`
2. Verify `NRF_FQDN` and `NRF_PORT` environment variables point to the correct NRF address.
3. Ensure the NRF container is running: `docker ps | grep nrf`

---

### NEF returns 429 Too Many Requests

**Symptom**: API calls return `429` with `{"title": "Too Many Requests", ...}`.

**Cause**: The global rate limiter threshold has been exceeded. The rate limiter is enforced before authentication, so it applies to every incoming request regardless of AF identity.

**Fix**:
- Reduce the aggregate request rate from all AFs.
- Note that the rate limit is global — a single high-traffic AF can exhaust the token budget for all other AFs.
- See [Resilience Guide](resilience.md#rate-limiter) for the rate limit configuration parameters.

---

### Subscription Expired — AF receives 404

**Symptom**: AF calls `GET /subscriptions/{id}` and receives `404 Not Found`. Notifications have stopped arriving.

**Cause**: The subscription's `monitorExpireTime` elapsed. NEF's `task_manager` automatically deleted the subscription and unsubscribed from AMF/PCF when the timer fired. The log entry `[task_manager] subscription expired` confirms this.

**Fix**:
- Re-subscribe using a new POST request.
- When creating the new subscription, set `monitorExpireTime` to a date further in the future, or omit the field entirely for a non-expiring subscription.
- To avoid losing subscriptions in production, implement renewal logic on the AF side: before the expiry timestamp, call PUT to update `monitorExpireTime`, or track expiry client-side and re-POST before it elapses.

---

### Notifications Stopped Unexpectedly

**Symptom**: The subscription still exists (`GET` returns `200 OK`) but notifications have stopped arriving at the AF.

**Cause A — Subscription expired in a narrow race window**

The `monitorExpireTime` elapsed in the brief interval between your GET and the notification stop.

**Fix**: Issue another GET on the subscription ID. If it returns `404`, the subscription expired — re-subscribe as described above.

**Cause B — Southbound NF circuit breaker is OPEN**

NEF cannot forward event notifications to the AF because it cannot reach the upstream NF (AMF/PCF) to receive them.

**Fix**: Check NEF logs for circuit breaker state on the relevant NF: `docker logs oai-nef 2>&1 | grep circuit_breaker`

**Cause C — `notificationURI` is unreachable from inside the NEF container**

NEF resolves the `notificationURI` from inside its Docker network. A hostname that resolves on the host machine may not resolve inside the container.

**Fix**:
1. Verify the AF's callback endpoint is reachable from inside the NEF container:
   ```bash
   docker exec oai-nef curl --http2-prior-knowledge -v http://your-af:8000/notify
   ```
2. Check that the AF container is on the same Docker network as NEF, or that DNS resolution works correctly inside the NEF container.
3. See [Deployment Guide](deployment.md) for Docker network configuration guidance.

---

### NEF container exits immediately at startup

**Symptom**: `docker ps` shows the container is not running. `docker logs oai-nef` shows a startup error.

**Cause A — Config file missing or malformed**

NEF reads `/openair-nef/etc/config.yaml` at startup. If the file is not mounted or contains invalid YAML, the process exits.

**Fix**:
1. Verify the volume mount is correct in your `docker run` or Compose file (e.g., `-v $(pwd)/etc/config.yaml:/openair-nef/etc/config.yaml`).
2. Validate the YAML syntax: `python3 -c "import yaml, sys; yaml.safe_load(open('etc/config.yaml'))"`.

**Cause B — Port 8080 already in use**

If another process on the host is bound to port 8080, NEF cannot start its HTTP/2 server.

**Fix**:
1. Identify the conflicting process: `lsof -i :8080`
2. Either stop the conflicting process or change the NEF port by setting the `NEF_INTERFACE_PORT_FOR_SBI` environment variable to an unused port and updating the `-p` Docker port mapping accordingly.

---

### NRF Registration Failure

**Symptom**: NEF logs show `NRF registration failed`. The `nfStatus` field in the `/health` response is `UNDISCOVERABLE`.

**Cause**: NRF is not reachable at the address NEF was configured to use.

**Fix**:
1. Ensure NRF container is running: `docker ps | grep nrf`
2. Verify `NRF_FQDN` resolves inside the NEF container:
   ```bash
   docker exec oai-nef nslookup $NRF_FQDN
   ```
3. Verify `NRF_PORT` is correct (default `8000` for OAI NRF).
4. Check NRF logs for errors: `docker logs oai-nrf 2>&1 | tail -50`

---

### Authentication Errors (401 / 403)

**Symptom**: API call returns `401 Unauthorized` or `403 Forbidden`.

**Case 1 — 401 Unauthorized**

Cause: No Bearer token was provided, or the JWT is invalid or expired.

Fix:
- If `insecure_dev_mode: false` (default), all APIs require a valid JWT signed with `jwt_secret`. Obtain a token from AUSF.
- Verify the token is not expired by inspecting the `exp` claim (e.g., via [jwt.io](https://jwt.io)).
- Include the `Authorization: Bearer <token>` header in every request.

**Case 2 — 403 Forbidden**

Cause: The AF's identity is not in the `af_list` whitelist in `config.yaml`, or the JWT's `aud` claim does not include `"nef"`.

Fix:
- Add the AF client ID to the `af_list` section of `config.yaml` and restart NEF (or reload config if hot-reload is supported).
- Verify the JWT's `aud` claim includes `"nef"`.

**Dev shortcut**: Set `insecure_dev_mode: true` in `config.yaml` (or the equivalent environment variable) to bypass all authentication and whitelist checks. **Never use `insecure_dev_mode` in production.**

See [API Reference Overview](api-reference/overview.md) for the full authentication model.

---

### HTTP/2 Connection Issues

**Symptom**: `curl` returns `curl: (1) Received HTTP/0.9 when not allowed` or a similar protocol error.

**Cause**: NEF uses HTTP/2 cleartext (h2c) exclusively. Standard `curl` defaults to HTTP/1.1 and is rejected by the NEF server.

**Fix**: Use `--http2-prior-knowledge` with every `curl` command:

```bash
curl --http2-prior-knowledge http://localhost:8080/health
```

For AF code, ensure the HTTP client library is configured to use HTTP/2 Prior Knowledge (direct h2c connection), not HTTP/1.1 or TLS-based h2. NEF does not provide TLS in this release — do not use `https://` URLs.

See [API Reference Overview](api-reference/overview.md#http-protocol-requirements) for supported protocol details.

---

### PFD Transaction Partially Applied

**Symptom**: `PUT /3gpp-pfd-management/v1/.../transactions/{transId}` returns `400` or `500`. Some application entries were written to UDR, others were not.

**Cause**: A UDR write failed mid-transaction. NEF should roll back on failure, but in exceptional error paths a partial state can remain.

**Fix**:
1. Issue a `DELETE` on the entire transaction to clean up the partial state:
   ```
   DELETE /3gpp-pfd-management/v1/{scsAsId}/transactions/{transId}
   ```
2. Re-attempt the `PUT` with all applications in a single request.

---

## Log Reference

Use the following patterns with `grep` to quickly classify issues:

| Log Pattern | Meaning |
|---|---|
| `[NRF] Registration successful` | NEF registered with NRF at startup |
| `[NRF] Heartbeat sent` | Periodic NRF heartbeat — confirms ongoing NRF connectivity |
| `[circuit_breaker] NF_TYPE OPEN` | Circuit breaker tripped for that NF type; southbound calls to it are rejected |
| `[circuit_breaker] NF_TYPE HALF_OPEN probe` | Circuit is testing whether the NF has recovered |
| `[circuit_breaker] NF_TYPE CLOSED` | Circuit recovered; normal traffic resumed |
| `[task_manager] subscription expired` | `monitorExpireTime` elapsed; subscription deleted automatically |
| `[rate_limiter] 429 sent` | Incoming request rate exceeded the global threshold |
| `[auth] JWT validation failed` | Authentication failure — see the log detail for the specific reason |

---

## Related Guides

- [Resilience Guide](resilience.md) — Circuit breaker states, rate limiter behaviour, and in-memory state restart implications
- [Deployment Guide](deployment.md) — Environment variables, Docker configuration, and container networking
- [API Reference Overview](api-reference/overview.md) — HTTP/2 requirements, authentication model, and error response format
