# NEF Configuration Reference

This page documents every parameter in the NEF configuration file. Read this page before deploying NEF in any environment other than the default Docker Compose setup.

---

## Configuration File Location & Format

| Property | Value |
|----------|-------|
| **Default path** | `etc/config.yaml` (relative to the repository root) |
| **Container path** | `/openair-nef/etc/config.yaml` |
| **Format** | YAML |
| **Hot reload** | Not supported — a process restart is required after any change |

The configuration file is loaded once at startup. The path can be overridden with the `--config` command-line flag:

```bash
oai_nef --config /path/to/custom-config.yaml
```

---

## Full Annotated Configuration Example

The block below reproduces the default `etc/config.yaml` with inline comments explaining every key.

```yaml
################################################################################
# OAI-CN5G-NEF Configuration File
################################################################################

############# Common configuration

log_level:
  # Logging verbosity for all NEF subsystems.
  # Accepted values: debug | info | warn | error
  # Use "info" or "warn" in production to reduce log volume.
  general: debug

# When set to "yes", NEF registers itself with the NRF on startup and
# deregisters on graceful shutdown.  Set to "no" only in isolated test
# environments where no NRF is present.
register_nf:
  general: yes

# HTTP protocol version used for ALL SBI connections (both listening and
# outbound client calls).  Valid values: 1 (HTTP/1.1) or 2 (HTTP/2 h2c).
# HTTP/2 is strongly recommended and is required by most 5GC deployments.
http_version: 2

############## SBI Interfaces — peer network functions

nfs:
  nrf:
    # FQDN or IPv4 address of the NRF.  Used for registration, heartbeat,
    # and NF discovery.
    host: oai-nrf
    sbi:
      port: 8080               # TCP port of the NRF SBI interface
      api_version: v1          # API version string included in request paths
      interface_name: eth0     # Local network interface used for outbound calls

  amf:
    # FQDN or IPv4 address of the AMF.  NEF subscribes to AMF event
    # exposure (Namf_EventExposure) for monitoring event subscriptions.
    host: oai-amf
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0

  smf:
    # FQDN or IPv4 address of the SMF.  Used for session-related event
    # subscriptions (Nsmf_EventExposure).
    host: oai-smf
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0

  pcf:
    # FQDN or IPv4 address of the PCF.  NEF forwards Traffic Influence
    # and BDT policy requests to PCF (Npcf_PolicyAuthorization).
    host: oai-pcf
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0

  udr:
    # FQDN or IPv4 address of the UDR.  NEF stores and retrieves PFD
    # data in the UDR (Nudr_DataRepository).
    host: oai-udr
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0

############## NEF-specific configuration

nef:
  # FQDN or IPv4 address that NEF advertises to the NRF and in
  # subscription self-links returned to AFs.
  host: oai-nef

  sbi:
    port: 8080           # TCP port NEF listens on for all northbound API calls
    api_version: v1      # API version string; currently only v1 is supported
    # Network interface NEF binds to.  Use "lo" for loopback-only testing,
    # "eth0" (or equivalent) for reachable deployments.
    interface_name: lo

  # Comma-separated list of northbound services to activate at startup.
  # Removing a service name from this list disables the corresponding API
  # routes.  All six services are enabled by default.
  support_features: >
    nnef-eventexposure,
    nnef-pfdmanagement,
    nnef-trafficinfluence,
    nnef-bdtpolicycontrol,
    nnef-qosmonitoring,
    nnef-analyticsexposure

  # AF/SCS whitelist.  When this list is non-empty, only AFs whose af_id
  # appears in the list are permitted to call the NEF.  An empty list ([])
  # means "allow all callers" — this is only safe when insecure_dev_mode
  # is enabled (see below).
  af_whitelist: []

  security:
    # HMAC-SHA256 secret used to sign and validate JWT Bearer tokens.
    # MUST be set to a cryptographically random string of at least 32
    # characters in any non-development deployment.
    # An empty string ("") disables JWT validation entirely.
    jwt_secret: ""

    # When true AND jwt_secret is empty AND af_whitelist is empty, NEF
    # operates in fail-open mode: all requests are accepted regardless of
    # authentication headers.  Intended for development and local testing
    # only.  Default is false (fail-closed).
    #
    # WARNING: NEVER set to true in production — all access is unrestricted.
    insecure_dev_mode: true
```

---

## Parameter Reference Table

### Logging

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `log_level.general` | string | `debug` | Logging verbosity for all NEF subsystems | One of: `debug`, `info`, `warn`, `error` |

### NRF Registration

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `register_nf.general` | bool | `yes` | Register NEF with NRF on startup; deregister on graceful shutdown | `yes` or `no` |

### HTTP Version

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `http_version` | int | `2` | HTTP protocol version for all SBI connections | `1` (HTTP/1.1) or `2` (HTTP/2 h2c) |

### Peer NF Endpoints — NRF

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nfs.nrf.host` | string | `oai-nrf` | FQDN or IPv4 address of the NRF | Non-empty string |
| `nfs.nrf.sbi.port` | int | `8080` | SBI TCP port of the NRF | 1–65535 |
| `nfs.nrf.sbi.api_version` | string | `v1` | API version string for NRF requests | Non-empty string |
| `nfs.nrf.sbi.interface_name` | string | `eth0` | Local interface for outbound NRF calls | Existing network interface name |

### Peer NF Endpoints — AMF

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nfs.amf.host` | string | `oai-amf` | FQDN or IPv4 address of the AMF | Non-empty string |
| `nfs.amf.sbi.port` | int | `8080` | SBI TCP port of the AMF | 1–65535 |
| `nfs.amf.sbi.api_version` | string | `v1` | API version string for AMF requests | Non-empty string |
| `nfs.amf.sbi.interface_name` | string | `eth0` | Local interface for outbound AMF calls | Existing network interface name |

### Peer NF Endpoints — SMF

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nfs.smf.host` | string | `oai-smf` | FQDN or IPv4 address of the SMF | Non-empty string |
| `nfs.smf.sbi.port` | int | `8080` | SBI TCP port of the SMF | 1–65535 |
| `nfs.smf.sbi.api_version` | string | `v1` | API version string for SMF requests | Non-empty string |
| `nfs.smf.sbi.interface_name` | string | `eth0` | Local interface for outbound SMF calls | Existing network interface name |

### Peer NF Endpoints — PCF

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nfs.pcf.host` | string | `oai-pcf` | FQDN or IPv4 address of the PCF | Non-empty string |
| `nfs.pcf.sbi.port` | int | `8080` | SBI TCP port of the PCF | 1–65535 |
| `nfs.pcf.sbi.api_version` | string | `v1` | API version string for PCF requests | Non-empty string |
| `nfs.pcf.sbi.interface_name` | string | `eth0` | Local interface for outbound PCF calls | Existing network interface name |

### Peer NF Endpoints — UDR

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nfs.udr.host` | string | `oai-udr` | FQDN or IPv4 address of the UDR | Non-empty string |
| `nfs.udr.sbi.port` | int | `8080` | SBI TCP port of the UDR | 1–65535 |
| `nfs.udr.sbi.api_version` | string | `v1` | API version string for UDR requests | Non-empty string |
| `nfs.udr.sbi.interface_name` | string | `eth0` | Local interface for outbound UDR calls | Existing network interface name |

### NEF SBI Parameters

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nef.host` | string | `oai-nef` | FQDN or IPv4 address advertised to NRF and in API self-links | Non-empty string |
| `nef.sbi.port` | int | `8080` | TCP port NEF listens on for northbound API calls | 1–65535 |
| `nef.sbi.api_version` | string | `v1` | API version string; included in all northbound paths | Non-empty string |
| `nef.sbi.interface_name` | string | `lo` | Network interface NEF binds to for the listening socket | Existing network interface name |

### Support Features

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nef.support_features` | string | All six services | Comma-separated list of northbound services to activate | One or more of: `nnef-eventexposure`, `nnef-pfdmanagement`, `nnef-trafficinfluence`, `nnef-bdtpolicycontrol`, `nnef-qosmonitoring`, `nnef-analyticsexposure` |

### AF Whitelist

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nef.af_whitelist` | list | `[]` (empty) | List of authorised AF entries; empty list allows all callers | YAML sequence |
| `nef.af_whitelist[*].af_id` | string | — | Unique identifier for the AF (required per entry); matched against the JWT `sub` claim or `X-API-Key` header | Non-empty string; exact match |
| `nef.af_whitelist[*].api_key` | string | `""` | Optional pre-shared API key; if set, the `X-API-Key` header must match | Any string; omit or leave empty to disable API key check for this AF |
| `nef.af_whitelist[*].allowed_apis` | list | `[]` (empty = all) | Optional list of service names this AF is permitted to call; empty list grants access to all enabled services | Zero or more of: `monitoring_event`, `traffic_influence`, `pfd_management`, `bdt_policy`, `qos_monitoring`, `analytics_exposure` |

### Security Parameters

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nef.security.jwt_secret` | string | `""` | HMAC-SHA256 secret for JWT token signing and validation; empty string disables JWT validation | At least 32 random characters in production; never commit a real secret to version control |
| `nef.security.insecure_dev_mode` | bool | `false` | When `true`, NEF is fail-open — all requests pass when no JWT secret and no AF whitelist are configured | `true` or `false`; must be `false` in any non-development environment |

> **Warning:** Setting `insecure_dev_mode: true` disables authentication enforcement entirely when no JWT secret and no AF whitelist are configured. **Never use this setting in production.** All requests — including unauthenticated ones — will be accepted.

### Async Dispatch

| Parameter Path | Type | Default | Description | Constraints |
|---|---|---|---|---|
| `nef.use_async_dispatch` | bool | `true` | Legacy compatibility key. NEF HTTP handlers always use the dispatcher/adapter path; omit the key or set it to `true`. | `false` is rejected during config parsing |
| `nef.dispatcher_pool_size` | uint | `0` | Optional dispatcher worker pool size override. `0` keeps the automatic size (`http_workers + 2`). | `0` or a positive integer |

> **Note:** The compile-time `NEF_DISABLE_ASYNC_DISPATCH` and runtime inline `use_async_dispatch: false` modes were removed. Stale configs that set `use_async_dispatch: false` fail clearly instead of silently changing request execution.
>
> **Pool sizing:** If the dispatcher pool has fewer workers than the HTTP pool, NEF logs a warning at startup. The warning is advisory; the pool still starts and all dispatched tasks will run.
>
> **Queue back-pressure:** If all dispatcher workers are busy and the internal task queue is full, NEF returns `503 Service Unavailable` to the caller immediately. This is intentional — it protects the dispatcher from unbounded memory growth under extreme load. Tune `dispatcher_pool_size` or reduce upstream request rate if you see frequent 503s.

See [Call Flows §6](call-flows.md#6-async-dispatch-nef_app_adapter) for sequence diagrams of both Option A and Option B delivery modes.

---

## AF Whitelist Example

The following YAML block shows a whitelist with two AF entries. Uncomment and add to `etc/config.yaml` under the `nef:` section to restrict access:

```yaml
nef:
  af_whitelist:
    - af_id: "my-af-1"
      api_key: "s3cr3tK3y-changeme"
      allowed_apis:
        - monitoring_event
        - traffic_influence

    - af_id: "my-af-2"
      # No api_key: the X-API-Key header is not required for this AF.
      # No allowed_apis: this AF is permitted to call all enabled services.
```

In this example:
- `my-af-1` must present the `X-API-Key: s3cr3tK3y-changeme` header and may only call the Monitoring Event and Traffic Influence APIs.
- `my-af-2` requires no API key and may call any enabled service.

---

## Hot Reload Limitations

> **Note:** Configuration changes require a full process restart. NEF reads `config.yaml` once at startup. There is no `SIGHUP`-triggered reload or live configuration update mechanism. After changing any configuration parameter, restart the NEF container or process for the change to take effect.

Subscriptions are held in memory and will be lost on restart. AFs must re-subscribe after a NEF restart.

---

## Docker Environment Variables

When deploying with Docker or Docker Compose, the entrypoint script maps container environment variables to the corresponding `config.yaml` parameters. The mapping is shown in the table below.

| Environment Variable | Mapped Config Parameter | Example Value | Description |
|---|---|---|---|
| `NEF_INTERFACE_NAME_FOR_SBI` | `nef.sbi.interface_name` | `eth0` | Network interface NEF listens on |
| `NEF_INTERFACE_PORT_FOR_SBI` | `nef.sbi.port` (HTTP/1.1) | `80` | HTTP/1.1 listening port |
| `NEF_INTERFACE_HTTP2_PORT_FOR_SBI` | `nef.sbi.port` (HTTP/2) | `9090` | HTTP/2 listening port |
| `NEF_API_VERSION` | `nef.sbi.api_version` | `v1` | API version string |
| `INSTANCE` | Instance identifier | `0` | Used to distinguish multiple instances in log output |
| `PID_DIRECTORY` | PID file directory | `/var/run` | Directory for the PID file |
| `AMF_IPV4_ADDRESS` | `nfs.amf.host` | `192.168.28.194` | AMF IPv4 address (used when `USE_FQDN_DNS=no`) |
| `AMF_PORT` | `nfs.amf.sbi.port` (HTTP/1.1) | `80` | AMF HTTP/1.1 port |
| `AMF_HTTP2_PORT` | `nfs.amf.sbi.port` (HTTP/2) | `9090` | AMF HTTP/2 port |
| `AMF_API_VERSION` | `nfs.amf.sbi.api_version` | `v1` | AMF API version |
| `AMF_FQDN` | `nfs.amf.host` | `cicd-oai-amf` | AMF hostname (used when `USE_FQDN_DNS=yes`) |
| `SMF_IPV4_ADDRESS` | `nfs.smf.host` | `192.168.28.195` | SMF IPv4 address |
| `SMF_PORT` | `nfs.smf.sbi.port` (HTTP/1.1) | `80` | SMF HTTP/1.1 port |
| `SMF_HTTP2_PORT` | `nfs.smf.sbi.port` (HTTP/2) | `9090` | SMF HTTP/2 port |
| `SMF_API_VERSION` | `nfs.smf.sbi.api_version` | `v1` | SMF API version |
| `SMF_FQDN` | `nfs.smf.host` | `cicd-oai-smf` | SMF hostname |
| `UDM_IPV4_ADDRESS` | `nfs.udr.host` | `192.168.28.199` | UDR/UDM IPv4 address |
| `UDM_PORT` | `nfs.udr.sbi.port` (HTTP/1.1) | `80` | UDR/UDM HTTP/1.1 port |
| `UDM_HTTP2_PORT` | `nfs.udr.sbi.port` (HTTP/2) | `9090` | UDR/UDM HTTP/2 port |
| `UDM_API_VERSION` | `nfs.udr.sbi.api_version` | `v1` | UDR/UDM API version |
| `UDM_FQDN` | `nfs.udr.host` | `cicd-oai-udm` | UDR/UDM hostname |
| `USE_FQDN_DNS` | Selects host vs. IP for peer NFs | `no` | When `yes`, FQDN variables are used; when `no`, IPv4 address variables are used |
| `USE_HTTP2` | Selects HTTP port mapping | `no` | When `yes`, HTTP/2 ports are active; when `no`, HTTP/1.1 ports are used |

> **Note:** The Docker Compose template in `ci-scripts/docker-compose/docker-compose.tplt` demonstrates all of the above environment variables in context. Use that file as the starting point for a production Compose deployment.
