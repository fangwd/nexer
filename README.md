# Nexer

Start and manage a development environment or server stack with one command.
Nexer runs services, checks readiness, orders dependencies, captures logs, and
provides CLI and HTTP controls. Optional TCP proxies prepare upstream apps
before forwarding traffic.

## Quickstart

Requires C++17, CMake 3.26+, libcurl and SQLite development libraries, Git, npm,
and make. The first build downloads libuv, llhttp, and jsini.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target nexer
```

Create `nexer.conf` beside your `api/` and `web/` projects. This example assumes
`npm run dev` listens on ports 3001 and 3000 respectively, with `nc` installed
for readiness checks:

```json
{
  "admin": { "enabled": true, "bind": "127.0.0.1", "port": 8301 },
  "apps": [
    {
      "name": "api",
      "command": { "file": "npm", "args": ["run", "dev"], "cwd": "./api" },
      "checker": { "file": "nc", "args": ["-z", "127.0.0.1", "3001"], "timeout": 1000 },
      "max_start_time": 60000
    },
    {
      "name": "web",
      "command": { "file": "npm", "args": ["run", "dev"], "cwd": "./web" },
      "checker": { "file": "nc", "args": ["-z", "127.0.0.1", "3000"], "timeout": 1000 },
      "depends_on": ["api"],
      "auto_start": true,
      "max_start_time": 60000
    }
  ]
}
```

Set an admin token, validate, and start:

```bash
export NEXER_ADMIN_TOKEN='replace-with-your-secret'
./build/nexer validate
./build/nexer
```

Nexer starts the API before the web app and stays in the foreground. Ctrl-C or
SIGTERM shuts down owned processes and their descendants. Startup failures
clean up and exit 1. Duplicate instances using the same config are rejected.

See [nexer.conf.example](nexer.conf.example) for a Docker Compose/PostgreSQL
example. Adjust project paths, commands, and ports before using either config.

## Everyday commands

In another terminal, set the same `NEXER_ADMIN_TOKEN` and use the same config:

```bash
./build/nexer status
./build/nexer logs web --follow
./build/nexer start api --wait
./build/nexer stop api --wait
./build/nexer restart api --wait
./build/nexer shutdown
```

Controls require the admin listener; `validate` only parses the config and
starts nothing. `--wait` waits for the operation result and exits nonzero on
failure. A deliberate stop blocks automatic restarts and proxy-triggered
starts until an explicit start or restart.

Use `--config /path/to/nexer.conf` **before** the command to select another
config. Otherwise Nexer searches `./nexer.conf`, then
`$XDG_CONFIG_HOME/nexer/nexer.conf`, then `$HOME/.config/nexer/nexer.conf` on Unix.

## Configuration essentials

Configs accept ordinary JSON and relaxed JSON syntax.

| App field | Purpose |
| --- | --- |
| `command` | Executable `file`, argument array `args`, working directory `cwd`, and `env: ["KEY=value"]`. |
| `auto_start` | Start this app and its dependencies when Nexer runs; defaults to false. |
| `depends_on` | Required app names; `preamble` is an alias. Use one spelling per app. |
| `checker` | Command whose zero exit means ready. A passing initial check skips launching the app. |
| `max_start_time` | Readiness polling budget in milliseconds; defaults to 0 (unlimited). |
| `restart` | `policy`: `never` (default), `on_failure`, or `always`, with optional backoff/retry settings. |
| `logs` | Optional `stdout_file` / `stderr_file`, `max_bytes` (default 10 MiB), and `max_files` (default 5 rotated copies). |

App types define what Nexer owns:

| `type` | Behavior |
| --- | --- |
| `service` (default) | Supervise a process; ready after its checker passes, or immediately after spawning without a checker. |
| `task` | Wait for a zero exit. Completed tasks satisfy dependencies; explicit start/restart reruns them. No checker or automatic restart. |
| `external` | Check/start a resource such as a container. Requires a checker; optional `stop_command` stops resources acquired by this run. No automatic restart. |

Relative paths resolve against the config directory; a relative executable
path containing `/` uses its command's `cwd` when set. Environment entries
replace inherited values. An already running resource found by a checker is
not adopted or stopped by Nexer. Checkers run during start/require operations,
not continuously.

All timeouts are milliseconds. Root `startup_timeout_ms` defaults to 180000;
0 disables that deadline. Automatic restarts default to five retries per
60000 ms, with delay doubling from 1000 ms to a 30000 ms cap. Exhaustion requires
an explicit start/restart. Override these with `restart.max_retries`,
`window_ms`, `delay_ms`, and `max_delay_ms`.

Log tails and audit history are in memory. App output can also go to rotating
files; create log directories beforehand. Files append across restarts and
rotate at the configured size. Optional root sections include `proxies`,
`logger`, `http`, and `forwarder`; see the [configuration types](runtime/include/nexer/runtime/config.h).

## HTTP API and server use

The [OpenAPI specification](docs/admin_openapi.yaml) describes the JSON API,
operation tracking, and SSE log streams. There is no bundled web UI.

- Admin access requires a nonempty token from `NEXER_ADMIN_TOKEN` by default;
  `admin.auth_token_env` or `admin.auth_token_file` selects another source.
- Requests use `X-Nexer-Admin-Token` by default. `/admin/healthz` is public
  liveness unless `health_require_auth` is enabled; `/admin/readyz` reports
  startup/service readiness and requires authentication.
- Keep the plaintext listener on loopback and use a reverse proxy for HTTPS.
  Native TLS is unsupported; `require_tls: true` fails startup. Child output
  is captured as-is, so applications must avoid printing secrets.

To install, run `cmake --install build --prefix /your/prefix`. For login/boot
startup, have launchd or systemd run `nexer --config /absolute/path/nexer.conf`
in the foreground with an explicit PATH, HOME, and token source. Allow enough
stop time for proxy draining and dependency cleanup; shutdown timing uses
`admin.shutdown.default_grace_ms` and `force_after_ms`.

## Development

```bash
cmake --build build --target run_tests
./build/run_tests
./build/run_tests process-manager   # one test group
```

Use separate builds for `-DNEXER_ASAN=ON` or `-DNEXER_VALGRIND=ON`; do not
combine them. `-DNEXER_CODE_COVERAGE=ON` enables coverage instrumentation.
macOS debug signing for `leaks` is controlled by `NEXER_SIGN_FOR_LEAKS`.
Run `./lint.sh` to format the C++ sources.

Reusable libraries are `nexer_core`, `nexer_http`, and `nexer_runtime`. See the
[runnable HTTP example](examples/http-server/main.cc) for embedding. Libuv
objects belong to their event loop thread; use `EventLoop::post` across threads
and copy callback buffers before retaining data. Initialize/clean up libcurl
when embedding HTTP clients.
