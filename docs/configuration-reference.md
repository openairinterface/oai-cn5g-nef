<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# NEF Configuration Reference

This page documents the parameters NEF actually reads from its YAML configuration file. Read it
before deploying NEF in any environment other than the default Docker Compose setup.

Every key described here was checked against the code that parses it: `config::read_from_file`
(`src/common-src/config/config.cpp`) for the common keys, and `nef_config_type::from_yaml`
(`src/nef_app/nef_config_types.cpp`) for the NEF-specific block. Keys that appear in the file but
are not read are called out as such.

For the authentication behaviour these keys drive, see the [Security Guide](security.md) and the
[API Overview](api-reference/overview.md#authentication). Both are the authoritative description of
how NEF authorizes a caller; this page covers only the configuration surface.

---

## File location and format

| Property | Value |
|----------|-------|
| Format | YAML, loaded once with `YAML::LoadFile`. libconfig is not used. |
| Container path | `/openair-nef/etc/config.yaml` |
| Repository template | `etc/config.yaml` |
| Config path flag | `-c` / `--config` (mandatory — NEF exits if it is missing) |
| Hot reload | Not supported. A process restart is required after any change. |

There is no built-in default path. The binary requires `-c`:

```bash
oai_nef -c /openair-nef/etc/config.yaml -o
```

The container runs exactly this command (`-o` sends logs to stdout). Other flags: `-r` writes a
rotating log file in the working directory, `-h` prints usage. These are the only flags the binary
accepts (`src/oai-nef/options.cpp`).

---

## Config structure at a glance

NEF configuration has two parts, and they live in different places in the file:

- **`nfs.*`** — the SBI endpoints of NEF and every peer NF (NRF, AMF, SMF, PCF, UDR). NEF's own
  listening host and port come from `nfs.nef`, exactly like the peers. This is the single most
  common point of confusion: there is no top-level `nef.host` or `nef.sbi`.
- **`nef.*`** — NEF-specific behaviour: the supported service list, the AF whitelist, the security
  block, and the optional dispatcher pool size.

A handful of common keys sit at the top level: `log_level`, `register_nf`, and the optional
`http_version` and `http_request_timeout`.

---

## Annotated configuration file

The block below reproduces the shipped `etc/config.yaml` with inline comments. It is the real file
structure, not a rearrangement.

```yaml
############# Common configuration

log_level:
  # Logging verbosity. Validated against a regex; use info or warn in production.
  general: debug

# Register with the NRF on startup so other NFs can discover NEF.
register_nf:
  general: yes

############## SBI Interfaces — NEF and its peers
# Every NF, including NEF itself, is configured here. NEF's own listening
# socket comes from nfs.nef.host and nfs.nef.sbi.*.
nfs:
  nrf:
    host: oai-nrf
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0
  amf:
    host: oai-amf
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0
  smf:
    host: oai-smf
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0
  pcf:
    host: oai-pcf
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0
  udr:
    host: oai-udr
    sbi:
      port: 8080
      api_version: v1
      interface_name: eth0
  nef:
    # NEF's own advertised host and listening SBI interface.
    host: oai-nef-test
    sbi:
      port: 8080           # port NEF listens on for northbound and Nnef calls
      api_version: v1      # version prefix in every path, e.g. /3gpp-monitoring-event/v1
      interface_name: eth0 # interface NEF binds its listening socket to

############## NEF-specific configuration
nef:
  # Comma-separated list of northbound services. Parsed and logged, but NOT
  # used to enable or disable routes — see the note below.
  support_features: >
    nnef-eventexposure,
    nnef-pfdmanagement,
    nnef-trafficinfluence,
    nnef-bdtpolicycontrol,
    nnef-qosmonitoring,
    nnef-analyticsexposure

  # AF/SCS allow-list. Empty (or omitted) means open access — safe only with
  # insecure_dev_mode: true. Each entry: af_id (required), api_key (optional,
  # see note), allowed_apis (optional).
  af_whitelist: []

  security:
    # HMAC-SHA256 secret used to VALIDATE inbound JWTs. NEF never issues
    # tokens. Empty disables JWT validation.
    jwt_secret: ""
    # Fail-open switch. When true AND no jwt_secret AND no af_whitelist are
    # set, NEF bypasses the allow-list and accepts every request. Default
    # false (fail-closed: deny everything when nothing is configured).
    insecure_dev_mode: true
```

The shipped file does not set `http_version`, so NEF defaults to HTTP/1.1. To serve cleartext
HTTP/2 (h2c), add `http_version: 2` at the top level.

---

## Common parameters

| Parameter path | Type | Default (built-in) | Description | Constraints |
|---|---|---|---|---|
| `log_level.general` | string | `info` | Logging verbosity for all subsystems | Validated by regex; `debug`, `info`, `warn`, `error` |
| `register_nf.general` | bool | `false` | Register with NRF on startup, deregister on shutdown | `yes` / `no` |
| `http_version` | string | `1.1` | HTTP version for the server and outbound clients | `1`, `1.1` (both HTTP/1.1) or `2` (HTTP/2 h2c) |
| `http_request_timeout` | uint | see code | Outbound SBI request timeout, seconds | Bounded by min/max in `config.hpp` |

Note the built-in defaults differ from the shipped template: the code default for `log_level` is
`info` and for `register_nf` is `false`, but `etc/config.yaml` sets `debug` and `yes`. The template
also omits `http_version`, so a container using it unchanged serves HTTP/1.1.

---

## Peer NF and NEF endpoints (`nfs.*`)

Each NF under `nfs` takes the same four keys. The table shows the built-in defaults; the shipped
template overrides every port to `8080` and sets `nfs.nef.host` to `oai-nef-test`.

| Parameter path | Type | Built-in default | Description |
|---|---|---|---|
| `nfs.nrf.host` | string | `oai-nrf` | NRF host (FQDN or IPv4). Used for registration and heartbeat. |
| `nfs.nrf.sbi.port` | int | `80` | NRF SBI port |
| `nfs.nrf.sbi.api_version` | string | `v1` | NRF API version prefix |
| `nfs.nrf.sbi.interface_name` | string | `eth0` | Local interface for outbound NRF calls |
| `nfs.amf.host` | string | `oai-amf` | AMF host |
| `nfs.amf.sbi.port` | int | `80` | AMF SBI port |
| `nfs.amf.sbi.api_version` | string | `v1` | AMF API version prefix |
| `nfs.amf.sbi.interface_name` | string | `eth0` | Local interface for outbound AMF calls |
| `nfs.smf.host` | string | `oai-smf` | SMF host |
| `nfs.smf.sbi.port` | int | `80` | SMF SBI port |
| `nfs.smf.sbi.api_version` | string | `v1` | SMF API version prefix |
| `nfs.smf.sbi.interface_name` | string | `eth0` | Local interface for outbound SMF calls |
| `nfs.pcf.host` | string | `oai-pcf` | PCF host |
| `nfs.pcf.sbi.port` | int | `80` | PCF SBI port |
| `nfs.pcf.sbi.api_version` | string | `v1` | PCF API version prefix |
| `nfs.pcf.sbi.interface_name` | string | `eth0` | Local interface for outbound PCF calls |
| `nfs.udr.host` | string | `oai-udr` | UDR host |
| `nfs.udr.sbi.port` | int | `80` | UDR SBI port |
| `nfs.udr.sbi.api_version` | string | `v1` | UDR API version prefix |
| `nfs.udr.sbi.interface_name` | string | `eth0` | Local interface for outbound UDR calls |
| `nfs.nef.host` | string | `oai-nef` | Host NEF advertises to NRF and in subscription self-links |
| `nfs.nef.sbi.port` | int | `80` | Port NEF listens on for northbound and Nnef calls |
| `nfs.nef.sbi.api_version` | string | `v1` | Version prefix in every NEF path |
| `nfs.nef.sbi.interface_name` | string | `eth0` | Interface NEF binds its listening socket to |

The built-in host defaults come from the `nef_config` constructor (`src/nef_app/nef_config.hpp`):
`oai-nef`, `oai-nrf`, `oai-amf`, `oai-smf`, `oai-pcf`, `oai-udr`. They apply only when the YAML
does not override them.

---

## NEF-specific parameters (`nef.*`)

### Support features

| Parameter path | Type | Built-in default | Description |
|---|---|---|---|
| `nef.support_features` | string | `nnef-eventexposure,nnef-pfdmanagement` | Comma-separated service list |

> **Not enforced.** `support_features` is parsed and stored, but nothing in the code reads it back:
> `get_support_features()` has no callers. Every implemented route is always active regardless of
> this list. Removing a name does not disable a route. (The template lists all six services; the
> built-in default lists only two.)

### AF whitelist

| Parameter path | Type | Default | Description |
|---|---|---|---|
| `nef.af_whitelist` | list | `[]` | Allow-list of AF entries. Empty means open access. |
| `nef.af_whitelist[*].af_id` | string | — | AF identifier. Compared, by exact string match, against the AF ID in the request URL path — or against the JWT `sub` claim when a valid bearer token is presented. Not compared against any header. |
| `nef.af_whitelist[*].api_key` | string | `""` | Parsed into the entry, but **not enforced**. No code reads an API-key header or calls `validate_api_key()`. See the note below. |
| `nef.af_whitelist[*].allowed_apis` | list | `[]` | Service names this AF may call. Empty means all services. Values must be the internal service names (see below). |

Allowed-API values are matched against the service constant passed by each handler
(`src/common/nef.h`). Use these exact strings:

| Service | `allowed_apis` value |
|---|---|
| Monitoring event / event exposure | `nnef-eventexposure` |
| Traffic influence | `nnef-trafficinfluence` |
| PFD management | `nnef-pfdmanagement` |
| BDT policy | `nnef-bdt` |
| QoS monitoring | `nnef-qosmonitoring` |
| Analytics exposure | `nnef-analyticsexposure` |

> **The `api_key` field does nothing today.** It is read into the whitelist entry and echoed in
> `to_json()`, but the request path never inspects an `X-API-Key` (or any other) header, and
> `nef_af_profile::validate_api_key()` is never called. Do not rely on it as an access control. This
> is consistent with the [Security Guide](security.md), which treats the AF allow-list (plus
> optional JWT validation) as the only authorization in force.

### Security

| Parameter path | Type | Default | Description |
|---|---|---|---|
| `nef.security.jwt_secret` | string | `""` | HMAC-SHA256 secret used to validate inbound JWTs. NEF never signs or issues tokens. Empty disables JWT validation. |
| `nef.security.insecure_dev_mode` | bool | `false` | When `true` and no `jwt_secret` and no `af_whitelist` are set, NEF bypasses the allow-list and accepts every request. |

`insecure_dev_mode` only matters when nothing else is configured. Its exact effect, and the
fail-closed default, are described in `authorize_af_request` (`src/nef_app/nef_app_core.cpp`) and
in the [Security Guide](security.md#insecure_dev_mode). The shipped template sets it to `true`; do
not deploy the template unchanged on any reachable network.

### Dispatcher pool size

| Parameter path | Type | Default | Description |
|---|---|---|---|
| `nef.dispatcher_pool_size` | uint | `0` | Async dispatcher worker-pool size. `0` keeps the automatic size (`http_workers + 2`). |

> **Removed knobs.** The compile-time `NEF_DISABLE_ASYNC_DISPATCH` and the old runtime
> `use_async_dispatch` mode are both gone, and they fail differently. Building with
> `-DNEF_DISABLE_ASYNC_DISPATCH` is a hard CMake error (`src/oai-nef/CMakeLists.txt`).
> `use_async_dispatch` in YAML is not read at all — the parser ignores unknown keys, so a stale
> `use_async_dispatch: false` starts normally and runs the dispatcher path anyway. Delete the key
> rather than expecting a warning.
>
> **Back-pressure.** When every dispatcher worker is busy and the task queue is full, NEF answers
> `503` with the ProblemDetails detail `"Server is overloaded, please retry later"`. This is
> deliberate load shedding. Raise `dispatcher_pool_size` or slow the upstream request rate if you
> see it often.

See [Call Flows §6](call-flows.md#6-async-dispatch-nef_app_adapter) for the dispatcher sequence
diagrams.

---

## Whitelist example

```yaml
nef:
  af_whitelist:
    - af_id: "my-af-1"
      allowed_apis:
        - nnef-eventexposure
        - nnef-trafficinfluence

    - af_id: "my-af-2"
      # No allowed_apis: this AF may call every service.
```

With this whitelist and no `jwt_secret`, authorization is by AF ID taken from the request URL path:

- `my-af-1` may call the monitoring-event/event-exposure and traffic-influence services only.
  A request to any other service returns `403` with detail `"AF not authorized for this service"`.
- `my-af-2` may call any service.
- Any other AF ID is rejected with the same `403`.

This is an allow-list keyed on the path AF ID, not authentication: without a `jwt_secret`, any
caller can put a listed AF ID in the URL. To require a verified credential, set `jwt_secret` and
have callers present a matching signed JWT — see the [Security Guide](security.md).

An `api_key` line is accepted here but has no effect, as noted above.

---

## Configuration changes require a restart

NEF reads the file once at startup. There is no `SIGHUP` reload and no live update path. Restart
the process or container after any change.

Subscriptions are held in memory and are lost on restart. AFs must re-subscribe afterward. See the
[Deployment Guide](deployment.md#8-known-operational-limitations).

---

## Environment variables (Docker)

The container entrypoint maps a set of environment variables onto config keys before starting NEF.
The mapping is done by `entrypoint.py`, which ships in the shared common-build tooling and is **not
part of this repository**, so the exact set cannot be verified from the NEF source alone. The
authoritative in-repo reference is `ci-scripts/docker-compose/docker-compose.tplt`, which
demonstrates the variables below.

| Environment variable | Maps to | Example | Notes |
|---|---|---|---|
| `NEF_INTERFACE_NAME_FOR_SBI` | `nfs.nef.sbi.interface_name` | `eth0` | Interface NEF listens on |
| `NEF_INTERFACE_PORT_FOR_SBI` | `nfs.nef.sbi.port` (HTTP/1.1) | `80` | HTTP/1.1 listen port |
| `NEF_INTERFACE_HTTP2_PORT_FOR_SBI` | `nfs.nef.sbi.port` (HTTP/2) | `9090` | HTTP/2 listen port |
| `NEF_API_VERSION` | `nfs.nef.sbi.api_version` | `v1` | API version prefix |
| `INSTANCE` | instance identifier | `0` | Distinguishes instances in logs |
| `PID_DIRECTORY` | PID file directory | `/var/run` | |
| `AMF_IPV4_ADDRESS` | `nfs.amf.host` | `192.168.28.194` | Used when `USE_FQDN_DNS=no` |
| `AMF_PORT` / `AMF_HTTP2_PORT` | `nfs.amf.sbi.port` | `80` / `9090` | HTTP/1.1 vs HTTP/2 port |
| `AMF_API_VERSION` | `nfs.amf.sbi.api_version` | `v1` | |
| `AMF_FQDN` | `nfs.amf.host` | `cicd-oai-amf` | Used when `USE_FQDN_DNS=yes` |
| `SMF_IPV4_ADDRESS` / `SMF_FQDN` | `nfs.smf.host` | `192.168.28.195` / `cicd-oai-smf` | |
| `SMF_PORT` / `SMF_HTTP2_PORT` | `nfs.smf.sbi.port` | `80` / `9090` | |
| `SMF_API_VERSION` | `nfs.smf.sbi.api_version` | `v1` | |
| `UDM_IPV4_ADDRESS` / `UDM_FQDN` | `nfs.udr.host` | `192.168.28.199` / `cicd-oai-udm` | The template uses `UDM_*` for the UDR peer |
| `UDM_PORT` / `UDM_HTTP2_PORT` | `nfs.udr.sbi.port` | `80` / `9090` | |
| `UDM_API_VERSION` | `nfs.udr.sbi.api_version` | `v1` | |
| `USE_FQDN_DNS` | selects `_FQDN` vs `_IPV4_ADDRESS` | `no` | |
| `USE_HTTP2` | selects the HTTP port pair | `no` | |

The shipped template sets variables for AMF, SMF and UDM only. It does not define NRF, PCF or UDR
variables — configure those peers through the mounted `config.yaml` under `nfs.*`. For anything
beyond the demonstrated variables, mount a full `config.yaml` rather than relying on the entrypoint.
