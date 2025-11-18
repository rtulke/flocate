# Repository Guidelines

## Project Structure & Module Organization
`plocate.cpp`, `lib.cpp`, and supporting headers in the repo root implement the CLI, compression pipeline, and posting-list plumbing. `updatedb.cpp` and the `plocate-updatedb.*` units handle database generation and systemd integration. Manual pages (`*.1`, `*.8`, `*.in`) and helper scripts (`update-plocate.sh`, `mkdir.sh`) live alongside the sources. Create an `obj/` build directory (kept out of version control) for all Meson artifacts, including tests and generated binaries.

## Build, Test, and Development Commands
- `meson setup obj` — Configure the project for C++17 with the default feature set; rerun with `meson setup obj --wipe` when dependencies change.
- `ninja -C obj` — Build `plocate`, `updatedb`, and helper tools.
- `sudo ninja -C obj install` — Install binaries, systemd units, and man pages (ensures the `plocate` system group exists).
- `ninja -C obj bench` — Exercise the TurboPFor benchmarks; run after touching compression code.
- `meson test -C obj` — Executes any Meson-defined tests; add new suites here when introducing features.

## Coding Style & Naming Conventions
All C++ code is formatted via `.clang-format`; run `clang-format -i file.cpp` before committing. Indentation uses hard tabs configured to a logical width of one indent level; alignment must rely on spaces. Favor descriptive `snake_case` for functions and variables, `CamelCase` for types, and keep headers self-contained. Target C++17, avoid compiler extensions, and guard optional features (eg, io_uring) behind capability checks in `conf.*`.

## Testing Guidelines
Extend or add Meson test targets under `meson.build`, and keep fixtures under `obj/` to avoid polluting the source tree. Tests should mirror the CLI they cover (eg, `plocate_skip_hidden`), and new compression logic must be validated with `ninja bench`. When modifying database generation, include regression tests that ingest a small synthetic dataset and assert locate results through a scriptable wrapper.

## Commit & Pull Request Guidelines
Commits follow the existing history: short, imperative summaries (eg, “Fix try_complete_pread() error on short read”). Group related changes, reference bug IDs or tracker links when applicable, and keep diffs minimal. Pull requests should explain the motivation, list manual and automated test results (`ninja -C obj`, `ninja bench`), mention configuration changes (systemd units, config files), and include screenshots or sample command output when altering user-visible behavior.

## Security & Configuration Notes
The locate database may expose file names; ensure `updatedb.conf.5.in` filters sensitive paths before shipping. Systemd units (`plocate-updatedb.service.in`, `.timer`) run as the `plocate` user—avoid introducing elevated operations outside that sandbox, and document any new capabilities in `README`.

## Communication & Language Guidelines
Day-to-day collaboration and discussions can stay in German, but keep code, inline comments, log/print strings, and all Markdown (including this guide) in English to maintain consistency with the existing codebase and documentation.
