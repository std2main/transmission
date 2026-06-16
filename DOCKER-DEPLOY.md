# Docker Deployment

This repository now includes a Docker-based deployment path for the modified `transmission-daemon`.

## What it does

- Builds the daemon directly from your current local source tree.
- Uses a multi-stage Docker build so the final image only contains runtime files, not the compiler toolchain or source tree.
- Uses `debian:bullseye-slim` for runtime compatibility with older Docker Engine 19.x hosts that can fail with newer Bookworm/glibc-based images.
- Persists Transmission state in `/config`.
- Persists payload data in `/downloads`.
- Exposes the quick-verify switches as environment variables so the container startup behavior is explicit.
- Pins `TRANSMISSION_WEB_HOME` to the bundled `/opt/transmission/web/public_html` so the Web UI is available in container builds.

## Default quick-verify behavior in `compose.yaml`

The provided compose file is tuned for your stated seeding workflow:

- `TRANSMISSION_QUICK_VERIFY_ENABLED=true`
- `TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED=false`

That means:

- newly added completed torrents use the sampled quick check
- if sampled data fails, the daemon does **not** automatically escalate into a full global verify

## Start the daemon

```bash
docker compose up --build -d
```

Open the Web UI at:

```text
http://localhost:9091
```

The bundled compose file only publishes the RPC/Web port to local host `127.0.0.1` by default.

## Stop the daemon

```bash
docker compose down
```

## Useful overrides

If you want full verification fallback on sampled mismatch:

```bash
docker compose run --rm \
  -e TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED=true \
  transmission
```

If you want to disable quick verify entirely:

```bash
docker compose run --rm \
  -e TRANSMISSION_QUICK_VERIFY_ENABLED=false \
  transmission
```

## Build a test-enabled image

```bash
docker build --build-arg ENABLE_TESTS=ON -t transmission-quick-check:test .
```

## Run the targeted quick-verify tests inside the image

```bash
docker run --rm transmission-quick-check:test \
  /src/build/tests/libtransmission/libtransmission-test \
  --gtest_filter=LT.TorrentTest.quickVerify*:LT.SettingsTest.can*QuickVerify*:LT.ApiCompatTest*
```

## Notes

- The container startup script updates `/config/settings.json` on each boot so that environment variables and persisted settings stay aligned.
- The compose example keeps the Web UI local to the host by binding `9091` to `127.0.0.1`. If you later expose it to your LAN, re-enable the RPC whitelist or add authentication first.
- If an older Docker host still crashes on startup with `std::system_error` / `Operation not permitted`, the host's default seccomp profile is usually too old for the runtime stack. In that case, first prefer upgrading Docker Engine; as a temporary workaround you can add `security_opt: ["seccomp=unconfined"]` to the compose service and test again.
