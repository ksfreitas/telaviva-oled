# Contracts: About Screen and Update Check

## Data Models

### Installed version
- Source of truth: `version` in `crates/telaviva-core/Cargo.toml` (semver `MAJOR.MINOR.PATCH`, optional `-prerelease`, no build metadata).
- Copies that must equal it (checked by a Rust test): `crates/telaviva-inference/Cargo.toml`, `native/macos/TelaViva/Info.plist` (`CFBundleShortVersionString` and `CFBundleVersion`), `native/windows/TelaViva/app.manifest` (`assemblyIdentity version="MAJOR.MINOR.PATCH.0"`).
- Runtime read: Windows from the core (`tv_core_version`); macOS from the bundle (`CFBundleShortVersionString`).

### UpdateOutcome (JSON produced by the core, UTF-8)
```json
{"status":"update_available","remote_version":"0.2.0","release_url":"https://github.com/ksfreitas/telaviva-oled/releases/tag/v0.2.0"}
```
| `status` | Meaning | Other fields |
|---|---|---|
| `update_available` | remote > installed | `remote_version`, `release_url` |
| `up_to_date` | remote <= installed | `remote_version` |
| `no_release` | HTTP 404, or the answer is a draft/pre-release | none |
| `rate_limited` | HTTP 403 or 429 | none |
| `http_error` | any other non-200 status | none |
| `invalid_response` | HTTP 200 but unusable body | none |

`release_url` is always built by the core as `https://github.com/ksfreitas/telaviva-oled/releases/tag/<tag_name>`; the `html_url` sent by the server is never used. `tag_name` must match `^[vV]?[0-9A-Za-z.+-]+$` after trimming, and `remote_version` is the tag without the leading `v`/`V`.

### UpdateState (shell-side, in memory only, never persisted)
`idle`, `checking`, `upToDate(remote)`, `updateAvailable(remote, url)`, `noRelease`, `rateLimited`, `failed` (transport error, `http_error`, `invalid_response`).

## API Contracts

### GitHub: latest release (consumed)
- Request: `GET https://api.github.com/repos/ksfreitas/telaviva-oled/releases/latest`
- Headers: `Accept: application/vnd.github+json`, `X-GitHub-Api-Version: 2022-11-28`, `User-Agent: TelaVivaOLED/<installed version>`. No `Authorization`, no cookies, no body, no query string, no other identifier. HTTPS only; HTTP is never attempted.
- Timeout: macOS 10 s total (`timeoutIntervalForResource`); Windows WinHTTP resolve 3 s, connect 3 s, send 3 s, receive 5 s. Response body capped at 1 MiB; a larger body is a failure.
- Relevant response fields (200): `tag_name` (string), `draft` (bool), `prerelease` (bool). All other fields are ignored.
- 404 = no published release; 403/429 = rate limit (60 requests/hour/IP unauthenticated).

### Core C ABI (additive to ABI version 1; `TV_CORE_ABI_VERSION` stays `1`)
```c
/* Writes the core version (UTF-8, no terminator) to out. Free with tv_buffer_free. */
int32_t tv_core_version(TvBuffer *out);

/* Compares the installed version with a GitHub releases/latest answer and writes the
   UpdateOutcome JSON to out. Free with tv_buffer_free. Pure: no network, no state. */
int32_t tv_update_evaluate(const uint8_t *installed, size_t installed_len,
                           uint32_t http_status,
                           const uint8_t *body, size_t body_len,
                           TvBuffer *out);
```
Return codes:
- `TV_OK`: `out` written (even for non-success outcomes such as `rate_limited`).
- `TV_INVALID_ARGUMENT`: null `out`, null pointer with non-zero length, empty `installed`, or `installed` not valid semver/UTF-8.
- `TV_SIZE_LIMIT`: `body_len` greater than 1 MiB or `installed_len` greater than 64 bytes.
- `TV_INTERNAL`: panic caught.
`out` is written only on `TV_OK`. `body` may be empty (`body_len == 0`, pointer may be null).

### Semver rules (core, `update.rs`)
- Accepts `MAJOR.MINOR.PATCH` of unsigned decimal numbers without leading zeros, optional `-prerelease` (dot-separated `[0-9A-Za-z-]+`), optional `+build` ignored, optional single leading `v`/`V` (tags only).
- Anything else (two or four parts, empty parts, signs, spaces) is malformed.
- Ordering: numeric comparison of the three numbers (`0.10.0 > 0.9.0`); with equal numbers a pre-release is lower than the release. A remote pre-release is never offered.

## Integration Contracts

### Shell to core
1. Shell obtains `installed` (macOS bundle string, Windows `tv_core_version`).
2. Shell performs the HTTPS GET, off the UI thread, obtaining `(status, body)` or a transport failure.
3. On transport failure the shell sets `failed` without calling the core.
4. Otherwise the shell calls `tv_update_evaluate(installed, status, body)` and maps `status` to `UpdateState`. An unknown `status` string is treated as `failed`.

### Shell to UI
User-visible text comes only from the shell string catalogs (ADR 0015). New keys (all in both locales on both platforms; `{version}` is replaced by the shell):

| Windows key (resw) | macOS key | en-US | pt-BR |
|---|---|---|---|
| `settings.tab.about` | `settings.tab.about` | About | Sobre |
| `tray.menu.about` | `menu.about` | About TelaViva OLED | Sobre o TelaViva OLED |
| `about.version` | `about.version` | Version | Versão |
| `about.project` | `about.project` | Project | Projeto |
| `about.credits.architect` | `about.credits.architect` | Architected by | Arquitetado por |
| `about.credits.coder` | `about.credits.coder` | Coded by | Codificado por |
| `about.credits.coders` | `about.credits.coders` | Claude and Codex | Claude e Codex |
| `about.update.title` | `about.update.title` | Updates | Atualizações |
| `about.update.checking` | `about.update.checking` | Checking for updates… | Verificando atualizações… |
| `about.update.upToDate` | `about.update.upToDate` | You have the latest version | Você está na versão mais recente |
| `about.update.available` | `about.update.available` | Update available: version {version} | Atualização disponível: versão {version} |
| `about.update.noRelease` | `about.update.noRelease` | No release has been published yet | Nenhuma versão foi publicada ainda |
| `about.update.rateLimited` | `about.update.rateLimited` | GitHub is limiting requests. Try again later. | O GitHub está limitando as consultas. Tente novamente mais tarde. |
| `about.update.failed` | `about.update.failed` | Could not check for updates | Não foi possível verificar atualizações |
| `about.update.checkNow` | `about.update.checkNow` | Check now | Verificar agora |
| `about.update.openRelease` | `about.update.openRelease` | Open release page | Abrir página da versão |

The architect name (`Cássio de Freitas e Silva`) and the product name (`TelaViva OLED`) are constants in code and are not translated. The coder line value contains a conjunction, so it is a key, `about.credits.coders` ("Claude and Codex" / "Claude e Codex"), present in both locales on both platforms.

## State Transitions

```
idle --About opened, no fresh result--> checking
idle --About opened, fresh result cached--> (cached state)
checking --200 + newer--> updateAvailable
checking --200 + same/older--> upToDate
checking --404--> noRelease
checking --403/429--> rateLimited
checking --transport error/other status/invalid body--> failed
any settled state --Check now (>= 30 s since last attempt)--> checking
any settled state --Check now (< 30 s)--> unchanged
```
Cache lifetime: `upToDate`, `updateAvailable` and `noRelease` are reused for 6 hours; `rateLimited` and `failed` are reused for 30 seconds. Nothing is written to disk; a restart starts at `idle`.
