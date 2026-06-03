# Design — `file.open` automation method (runtime model loading)

**Date:** 2026-06-03
**Branch:** `feature/automation`
**Status:** Approved for implementation

---

## Problem

Today a model can be loaded into OrcaSlicer **only at process launch**: the model path
is passed as a CLI positional arg and OrcaSlicer's normal startup file-loading ingests
it. The JSON-RPC automation protocol has **no** load method, so swapping or adding a
model in an already-running instance requires a fresh process launch.

Driving the native File→Import dialog via `input.click` is not a viable substitute: the
OS file picker is not a `wxWindow`, so it never appears in the `tree.dump` hierarchy
(`WxUiBackend::dump_tree` walks `wxGetApp().mainframe` children only), and `input.click`
can only target nodes resolved from that tree (no raw-coordinate click). Blind typing via
`input.type` is mechanically possible but unobservable: the native picker is not a
`wxDialog`, so `app.state().modal_dialog` and `sync.wait_for` cannot gate on it, leaving
only sleep-and-hope timing. A direct API method is the clean fix.

## Goal

Add a `file.open` JSON-RPC method that loads one or more files into a running instance by
calling `Plater::load_files(...)` directly on the GUI thread. Out of scope: any
dialog-driving mechanism (intercept hook or true OS-level drive) — explicitly deferred.

---

## Protocol

- **Method:** `file.open`
- **Params:** `{ "paths": ["C:/abs/a.stl", ...] }`
  - A bare string is also accepted: `{ "paths": "C:/abs/a.stl" }`.
  - Paths must be **absolute**. The server reads them from the host filesystem
    (client/server are localhost-only).
- **Result:** `{ "ok": true, "loaded": <count> }`
  - `count` is `load_files(...).size()` — the number of objects added to the scene.
- **Errors:**
  | Code | Constant | Condition |
  |------|----------|-----------|
  | -32602 | `kInvalidParams` | `paths` missing/empty, a non-string entry, or no non-empty path |
  | 1004 | `kErrGuiBusy` | GUI-thread marshal timed out (`m_gui_timeout_ms`) |
  | 1007 | `kErrLoadFailed` | `load_files` returned empty / threw (not found, parse error, unsupported format) — **new code** |

## Semantics — synchronous

`Plater::load_files` runs and completes on the GUI thread. The backend marshals via the
existing `run_on_gui(m_gui_timeout_ms, …)` helper and returns only after the load
finishes. Consequently, when `file.open` returns `ok:true`, `app.state().project_loaded`
is already `true` — there is no polling race.

Rejected alternative — async "fire-and-poll-`project_loaded`": adds client complexity and
loses a definitive per-call error result, with no benefit since loading is synchronous.

**Caveat:** an extremely large model could exceed `m_gui_timeout_ms` and surface as
`1004 kErrGuiBusy`. Documented; not mitigated in v1.

## Load strategy (v1 minimal)

Pass the default `LoadStrategy::LoadModel | LoadStrategy::LoadConfig` (identical to
drag-drop / `Plater::load_files`'s default) with `ask_multi = false`. This already routes
`.3mf` files as projects and meshes as models based on file content, so **no `as_project`
flag is needed in v1**. A future `{ "as_project": bool }` flag remains possible but is not
implemented now.

---

## Components / files to touch

Follows the existing `screenshot_window` / `app_state` method pattern.

1. **`src/slic3r/GUI/Automation/IUiBackend.hpp`** — add pure-virtual
   `int open_files(const std::vector<std::string>& paths)` returning the loaded count,
   throwing `AutomationError` on failure. Header stays wx-free (no `LoadStrategy` leak).
2. **`src/slic3r/GUI/Automation/WxUiBackend.{hpp,cpp}`** — implement `open_files`:
   `run_on_gui(m_gui_timeout_ms, …)` → `wxGetApp().plater()->load_files(paths, default_strategy, false)`;
   throw `kErrLoadFailed` if the returned vector is empty.
3. **`src/slic3r/GUI/Automation/JsonRpcDispatcher.{hpp,cpp}`** —
   - add `constexpr int kErrLoadFailed = 1007;`
   - declare + define `m_file_open(params)` (param parsing/validation; accept string or
     array; require ≥1 non-empty string path)
   - add dispatch route `if (method == "file.open") return make_result(id, m_file_open(params));`
   - add `"file.open"` to the capabilities array in `m_version`.
4. **`tests/automation/MockUiBackend.hpp`** — `open_files` override recording the paths
   vector + a configurable return-count (and a throw/fail knob).
5. **`tests/automation/test_dispatcher.cpp`** — Catch2 v2 tests:
   - array of paths → routes to backend, returns `loaded` count
   - bare-string path → normalized to one path
   - missing/empty `paths` → `-32602` (`kInvalidParams`)
   - backend load failure → `1007`
   - `automation.version` capabilities array includes `"file.open"`
6. **`tools/automation/orca_automation.py`** — `open(self, paths)` wrapper (normalize
   `str` → `[str]`, send `file.open`).
7. **`tools/automation/example_slice.py`** — launch **without** a model arg, then
   `orca.open([model])`, then wait for `project_loaded`.
8. **`doc/automation.md`** — document method (params/result/errors), add to the
   capabilities list, method index, and error table (`1007`).

---

## Testing / verification

- **Build (Windows):** `cmake --build . --config RelWithDebInfo --target ALL_BUILD -- -m`.
- **Unit:** `automation` Catch2 suite green including new tests (≈31 → ≈34 cases).
- **Manual:** launch with `--automation-server` and **no** model arg → call `file.open`
  → confirm `app.state().project_loaded` flips `true` and `screenshot.window` shows the
  model.
- **Gating:** unchanged — the server only runs under `--automation-server`, so the method
  is a no-op (unreachable) when automation is disabled.

## Backward compatibility

Additive only: a new method, a new error code, and a new capabilities entry. No change to
existing methods, profiles, project-file handling, or default behavior.
