# C4 contract: protocol and core-release boundaries

The protocol-to-Xray shape below is frozen against Xray-core **v26.9.9**, tag
`v26.9.9`, commit `52a412d`. Source references are pinned to that tag, not
rolling `main`:

- `infra/conf/vmess.go` (VMess outbound: exactly one `vnext` receiver and one user;
  `id`, `security`, `level`; Xray maps unknown VMess security to AUTO, so XRM
  will validate its own supported values rather than rely on that fallback).
- `infra/conf/trojan.go` (Trojan outbound: exactly one `servers` entry with
  address, port and password; non-empty `flow` is removed/unsupported).
- `infra/conf/shadowsocks.go` (SS outbound: exactly one server with address,
  port, method and password; Xray accepts classic AEAD and SS2022 methods).
- `infra/conf/hysteria.go`, `infra/conf/transport_internet.go`, and
  `infra/conf/transport_method.go` (outbound protocol `hysteria`; Hysteria
  transport under `streamSettings.hysteriaSettings`; transport version must be
  2 and supports `auth`, `udpIdleTimeout`, `masquerade`).

The official Xray output envelope is always an ordinary outbound object with
`tag`, `protocol`, `settings`, and applicable `streamSettings`. The current M3
stream variants remain the shared transport/TLS/REALITY representation. No
top-level protocol or transport files are introduced.

## Frozen URI scope

| Typed protocol | Accepted input | Fields consumed | Explicit unsupported boundary |
|---|---|---|---|
| VMess | `vmess://` base64 JSON share; standard and URL-safe base64, optional padding | `add`, `port`, `id`, `ps`, `scy`, `net`, `host`, `path`, `tls`, `sni`, `alpn`, `fp`; `aid` must be zero; absent `scy` means `auto`, otherwise only `auto`, `aes-128-gcm`, `chacha20-poly1305` | nonzero `aid`, any other `scy`, unknown transport/security values, unknown connection-affecting fields |
| Trojan | `trojan://password@host:port` URI | percent-decoded password; `type` raw/ws/grpc/httpupgrade/xhttp; corresponding `host`, `path`, `serviceName`, `authority`, `mode`; `security` none/tls; `sni`, `alpn`, `fp` | `flow`, plugin, unknown connection-affecting query fields |
| Shadowsocks | SIP002 URL-safe base64 userinfo; legacy whole-authority base64 form | `method`, `password`, endpoint, fragment name | every non-empty `plugin` value; malformed/unsupported method |
| Hysteria2 | `hysteria2://` and `hy2://` URI | endpoint, auth, `sni`, TLS selection and explicitly mapped Xray Hysteria transport fields | `obfs`/`obfs-password`, pinning, bandwidth controls, and all parameters without a v26.9.9 Xray mapping |

The inputs are URI/share formats, not general profile imports. Unknown
connection-affecting parameters fail as `unsupported`; they are not silently
discarded. Display-name fragments are decoded and retained. Codec fixture tests
must use separate hand-authored expected outbound JSON rather than deriving
expectations from the writer.

## Release/install DTO boundary

`CoreRelease` records the official release tag, publication time, prerelease
flag and assets. An asset carries its official name/URL, parsed platform and
architecture, plus the official SHA-256 when available. Install requests name
the exact release and asset; missing official checksum acceptance is an
explicit request flag and defaults to false. `core.install` returns an
operation; its completion carries the actual registered `CoreInstall`.
Installation never implies `core.use`.
The installer defaults release discovery to the official GitHub API URL; its
constructor accepts an explicit API URL so its HTTP boundary can be tested
against loopback fixtures without changing product defaults or contacting GitHub.
