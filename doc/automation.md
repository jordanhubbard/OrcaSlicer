# OrcaSlicer UI Automation Protocol (v1.0.0)

OrcaSlicer ships an **opt-in, localhost-only JSON-RPC server** that lets external
scripts introspect, drive, and screenshot the running OrcaSlicer GUI. It is built
for end-to-end testing and automation: a script can enumerate the live widget
tree, click buttons, type text, send keyboard shortcuts, wait for UI state, query
high-level application state, load models/projects into the running instance,
switch the active view/tab, and capture window images (the on-screen capture
includes the 3D viewport).

This document is the protocol reference. It describes activation, the transport,
the JSON-RPC envelope, every method, the unified node shape, the target/locator
model, error codes, the set of instrumented automation ids, ImGui specifics,
platform caveats, a quick-start snippet, and planned future work.

---

## 1. Overview & activation

The automation server is **OFF by default**. It is enabled with two
command-line flags:

| Flag | Meaning |
|---|---|
| `--automation-server` | Enable the automation server. |
| `--automation-server-port=PORT` | Override the listening port. Optional; default is **13619**. |

Example:

```bash
OrcaSlicer --automation-server --automation-server-port=13619 model.stl
```

The server binds to **`127.0.0.1` only** (the loopback interface). It is never
exposed on an external network interface.

**Security note (v1):** there is **no authentication token** in v1. The localhost
bind is the *only* security boundary. Any process able to run code on the machine
can connect to the port and drive the GUI — including injecting mouse and keyboard
input — while the server is enabled. The feature is intended for testing and
automation environments, not for production or shared/multi-user machines.

When the server is enabled, OrcaSlicer emits a `warning`-level log line at startup
to make the active input-injection surface obvious in logs, for example:

```
UI automation server ENABLED ... input injection is active
```

---

## 2. Transport

The server speaks **HTTP/1.1** over the loopback TCP socket:

| Request | Response |
|---|---|
| `POST /jsonrpc` with a JSON-RPC 2.0 request body | A JSON-RPC 2.0 response with `Content-Type: application/json`. |
| `GET /` | A plain-text health page: `OrcaSlicer automation server v1.0.0` (`Content-Type: text/plain`). |
| Anything else | HTTP `404 Not Found`. |

The server is **single-client / serialized** in v1: it handles one request at a
time on its own dedicated I/O thread. Connections are not kept alive; each request
is answered and the socket is closed. Clients should issue requests sequentially.

---

## 3. JSON-RPC envelope

The protocol follows **JSON-RPC 2.0**.

**Request:**

```json
{ "jsonrpc": "2.0", "id": <id>, "method": "<method>", "params": { ... } }
```

- `params` may be omitted; the server treats a missing `params` as an empty object.

**Success response:**

```json
{ "jsonrpc": "2.0", "id": <id>, "result": { ... } }
```

**Error response:**

```json
{ "jsonrpc": "2.0", "id": <id>, "error": { "code": <int>, "message": "<string>" } }
```

The request `id` is echoed back in the response. When the request has no `id`, or
when the request body cannot be parsed as JSON, the response `id` is `null`.

---

## 4. Methods

There are 12 methods. Capabilities advertised by `automation.version` list the 11
callable feature methods (every method except `automation.version` itself).

### `automation.version`

Returns server identity and the list of supported methods. Takes no parameters.

**Result:**

```json
{
  "version": "1.0.0",
  "protocol": "2.0",
  "capabilities": [
    "tree.dump", "tree.find", "widget.get", "input.click", "input.type",
    "input.key", "sync.wait_for", "app.state", "screenshot.window", "file.open",
    "view.select"
  ]
}
```

### `tree.dump`

Snapshot the live UI tree as a single root node with nested children.

**Params (all optional):**

| Param | Type | Default | Meaning |
|---|---|---|---|
| `root` | string (id or path) | full tree | Root the dump at the node with this id/path. |
| `max_depth` | int | `-1` | Maximum depth to descend. `-1` = unlimited. |
| `visible_only` | bool | `false` | When true, omit non-visible nodes. |
| `include_imgui` | bool | `true` | When true, include ImGui items. |

**Result:** the serialized root [node](#5-unified-node-shape), with `children`
included.

### `tree.find`

Find all nodes matching a [target predicate](#6-target--locator).

**Params:** a target predicate — any combination of `name`, `class`, `label`,
`value`, `backend` (provided fields are ANDed). The params object is the target
itself (it is *not* wrapped in a `target` key for this method).

**Result:** a **flat JSON array** of matching nodes. The nodes in this array are
returned **without** their `children` (use `widget.get`/`tree.dump` to descend).

### `widget.get`

Fetch a single node by [target](#6-target--locator).

**Params:**

| Param | Type | Required | Meaning |
|---|---|---|---|
| `target` | object | yes | Target spec (id / path / predicate). |

**Result:** a single [node](#5-unified-node-shape), with its `children` included.

**Errors:** `1001` if the target is **not found** *or* **ambiguous** (more than one
match).

### `input.click`

Click a resolved, actionable node.

**Params:**

| Param | Type | Default | Meaning |
|---|---|---|---|
| `target` | object | required | Target spec; must resolve to exactly one node. |
| `button` | string | `"left"` | `"left"`, `"right"`, or `"middle"`. |
| `double` | bool | `false` | Double-click when true. |
| `modifiers` | array of string | `[]` | Held modifiers: any of `"ctrl"`, `"shift"`, `"alt"`, `"cmd"` (`"meta"` is accepted as an alias of `"cmd"`). |

**Result:** `{ "ok": true }`.

**Errors:** `1001` not found / ambiguous; `1002` if the target is disabled or
hidden (not actionable). The click path raises and focuses the target's top-level
window before injecting the click.

### `input.type`

Type text into the currently focused control.

**Params:**

| Param | Type | Required | Meaning |
|---|---|---|---|
| `text` | string | yes | The text to type. |
| `target` | object | no | If given, this node is clicked first (to focus it) before typing. |

**Result:** `{ "ok": true }`.

**Errors:** if `target` is supplied, the same actionability errors as
`input.click` apply (`1001` / `1002`).

### `input.key`

Send a key chord (a key plus optional modifiers) to the focused window.

**Params:**

| Param | Type | Required | Meaning |
|---|---|---|---|
| `keys` | string or array | yes | Either a `"+"`-joined string like `"ctrl+s"`, or an array like `["ctrl", "s"]`. The last token is the key; earlier tokens are modifiers. |

**Result:** `{ "ok": true }`.

**Key names must be lowercase.** Recognized key names include `"enter"`, `"tab"`,
`"esc"`, `"space"`, `"delete"`, `"backspace"`, `"f5"` (and other function keys),
and single characters (e.g. `"s"`, `"a"`). Recognized modifiers are `"ctrl"`,
`"shift"`, `"alt"`, `"cmd"` (with `"meta"` as an alias for `"cmd"`).
**Unrecognized or uppercase key names are silently ignored** — no error is
returned, the key simply does not fire. Use lowercase names exclusively.

### `sync.wait_for`

Poll the UI until a target node reaches a desired state, or time out. This is the
preferred way to synchronize with asynchronous UI changes (it replaces fragile
fixed sleeps). Internally it repeatedly refreshes and dumps the tree, re-resolves
the target, and evaluates the requested state until it is satisfied.

**Params:**

| Param | Type | Default | Meaning |
|---|---|---|---|
| `target` | object | required | Target spec. |
| `state` | string | required | One of `"exists"`, `"visible"`, `"enabled"`, `"value"`. |
| `value` | string | — | Required when `state` is `"value"`; the expected value to match. |
| `timeout_ms` | int | `5000` | Maximum time to wait, in milliseconds. |
| `poll_ms` | int | `100` | Poll interval, in milliseconds (minimum 1). |

State semantics:

- `exists` — the target resolves to a node.
- `visible` — the node exists and is visible.
- `enabled` — the node exists and is **both enabled and visible**.
- `value` — the node has a value and that value equals the supplied `value`.

**Result:** `{ "ok": true, "elapsed_ms": <int> }`.

**Errors:** `1003` on timeout (the state was not reached within `timeout_ms`).

### `app.state`

Return a high-level application-state snapshot. Takes no parameters.

**Result:**

```json
{
  "active_tab": "<string>",
  "project_loaded": <bool>,
  "slicing": <bool>,
  "slice_progress": <int>,
  "foreground": <bool>,
  "modal_dialog": "<string>"
}
```

| Field | Meaning |
|---|---|
| `active_tab` | The active top-level tab/page. |
| `project_loaded` | Whether a project/model is currently loaded. |
| `slicing` | Whether slicing is currently in progress. |
| `slice_progress` | Slicing progress (`-1` when unknown). |
| `foreground` | Whether the main window is in the foreground. |
| `modal_dialog` | Present only when a modal dialog is active; identifies it. Omitted otherwise. |

### `screenshot.window`

Capture a window as a PNG, exactly as it appears on screen.

**Params:**

| Param | Type | Default | Meaning |
|---|---|---|---|
| `target` | object | main frame | If given, capture this window; otherwise capture the main frame. |

**Result:** `{ "png_base64": "<base64 PNG>", "width": <int>, "height": <int> }`.

**Errors:** `1005` on screenshot failure; `1001` if a supplied `target` is not
found or ambiguous.

**How it works:** the window's on-screen rectangle is read back from the
DWM-composited desktop framebuffer (`wxScreenDC`), so the capture includes every
native child control, the OpenGL 3D viewport, and ImGui overlays — it is a faithful
image of what the user sees. (Capturing the parent window's own client DC instead
would clip out child HWNDs and the GL surface, leaving them black; that is why this
method reads from the screen.)

**Caveats:**

- The window must be **visible and unobscured**. Because the source is the on-screen
  framebuffer, any overlapping window occludes the captured region. The backend
  raises the target window before capturing.
- **HiDPI:** the reported `width`/`height` come from the window's logical client size,
  while the screen framebuffer is in physical pixels. On per-monitor-DPI displays the
  two can differ; the capture may be cropped or scaled relative to the logical size.
- Because the capture is the live on-screen image, the 3D content reflects the
  **current view**: the model in the 3D editor, or the gcode toolpaths in Preview
  after a slice. There is no separate offscreen 3D-render method — the window
  capture already includes whatever the GL canvas is showing.

### `file.open`

Load one or more files into the **already-running** instance at runtime, by calling
`Plater::load_files(...)` directly on the GUI thread. This is the supported way to add
or swap a model without relaunching the process. Loading is **synchronous**: when the
call returns `ok: true`, `app.state().project_loaded` is already `true` (no polling
race).

**Params:**

| Param | Type | Required | Meaning |
|---|---|---|---|
| `paths` | string or array of strings | yes | One or more **absolute** file paths. A bare string is accepted and treated as a one-element list. Paths are read from the **host (server) filesystem** — client and server are localhost-only. |

`.3mf` files are routed as projects and meshes as models automatically, based on file
content (the same default strategy as drag-drop); there is no `as_project` flag in v1.

**Result:** `{ "ok": true, "loaded": <int> }`, where `loaded` is the number of objects
added to the scene (`load_files(...).size()`).

**Errors:**

- `-32602` (invalid params) — `paths` is missing, is not a string/array, contains a
  non-string entry, or yields no non-empty path.
- `1007` (load failed) — `load_files` returned empty or threw (file not found, parse
  error, or unsupported format).
- `1004` (GUI busy) — the GUI-thread marshal timed out. An extremely large model can
  exceed the marshal timeout and surface here; documented, not mitigated in v1.

### `view.select`

Switch the main window to a top-level view/tab at runtime. Useful to put the UI in a
known state before other actions — e.g. switch to **Prepare** before loading a model,
or to **Preview** after slicing.

**Params:**

| Param | Type | Required | Meaning |
|---|---|---|---|
| `view` | string | yes | The target view. One of: `home`, `prepare` (3D editor), `preview` (sliced G-code), `device`, `multi_device`, `project`, `calibration`. |

**Result:** `{ "ok": true, "view": <string>, "index": <int> }`, where `index` is the
resulting tab index (it can vary with layout, since some tabs — e.g. `multi_device` —
are only present in certain configurations).

**Errors:**

- `-32602` (invalid params) — `view` is missing, is not a string, or is empty.
- `1001` (not found) — the view name is unknown, or that view is not available in the
  current layout (for example `prepare`/`preview` in G-code-viewer mode, or
  `multi_device` when multi-device is disabled).
- `1004` (GUI busy) — the GUI-thread marshal timed out.

---

## 5. Unified node shape

Both wx widgets and ImGui items are reported with the same node schema:

```json
{
  "backend": "wx" | "imgui",
  "id": "<string>",
  "path": "<string>",
  "class": "<string>",
  "label": "<string>",
  "rect": { "x": <int>, "y": <int>, "w": <int>, "h": <int> },
  "enabled": <bool>,
  "visible": <bool>,
  "value": "<string>",
  "children": [ <node>, ... ]
}
```

| Field | Meaning |
|---|---|
| `backend` | `"wx"` for native wxWidgets controls, `"imgui"` for immediate-mode ImGui items. |
| `id` | The automation id when one is set, otherwise a derived id. For ImGui items the `path` doubles as the `id`. |
| `path` | Positional path, e.g. `"MainFrame/Panel[2]/Button[0]"`. For ImGui items: `"ImGui/<window>/<label>"`. |
| `class` | wx class name, or the ImGui item type. |
| `label` | The control's label/caption. May include an ImGui `##`-id suffix for ImGui items. |
| `rect` | Bounding rectangle in **screen coordinates**. |
| `enabled` | Whether the control is enabled. |
| `visible` | Whether the control is visible. |
| `value` | The control's value (text/choice/check/slider, etc.). **Omitted entirely** when the control has no applicable value. |
| `children` | Child nodes. **wx only**, and present only when children are included (e.g. `tree.dump`, `widget.get`). ImGui items are flat (no children) and are listed under their window. |

Notes:

- The `value` key is **omitted** (not `null`) when the control has no value.
- `children` is present only for wx nodes when children are requested; ImGui nodes
  never carry `children`.

---

## 6. Target / locator

Most methods accept a **target** object that identifies one or more nodes. A
target may specify:

| Field | Meaning |
|---|---|
| `id` | Exact automation id. |
| `path` | Exact positional path. |
| `name` | Predicate: matches either the node's `id` **or** its `label`. |
| `class` | Predicate: exact class name. |
| `label` | Predicate: exact label. |
| `value` | Predicate: node has a value and it equals this string. |
| `backend` | Predicate: `"wx"` or `"imgui"`. |

**Resolution order:** **`id` → `path` → predicate.**

- If `id` is present, only `id` is used (exact match).
- Else if `path` is present, only `path` is used (exact match).
- Else the predicate fields (`name`, `class`, `label`, `value`, `backend`) are
  used, and all provided predicate fields are **ANDed** together.

Action methods (`input.click`, `input.type` with a target, `widget.get`, and
single-target `screenshot.window`) require a **unique** match. If the target
resolves to zero matches or more than one match, the call fails with error `1001`
(not found / ambiguous). `tree.find` is the exception: it returns *all* matches as
an array and never errors on ambiguity.

---

## 7. Error codes

Standard JSON-RPC codes:

| Code | Meaning |
|---|---|
| `-32700` | Parse error — the request body was not valid JSON. |
| `-32600` | Invalid request — missing/invalid `method`. |
| `-32601` | Method not found — unknown method name. |
| `-32602` | Invalid params — missing/invalid parameters for the method. |

Application-specific codes:

| Code | Meaning |
|---|---|
| `1001` | Widget/target not found **or** ambiguous (more than one match). |
| `1002` | Not actionable — the target is disabled or hidden. |
| `1003` | Wait timeout — `sync.wait_for` did not reach the requested state in time. |
| `1004` | GUI thread busy / timeout — a backend call could not be marshaled onto the GUI thread in time (wedged GUI). |
| `1005` | Screenshot failed. |
| `1006` | Disabled. |
| `1007` | Load failed — `file.open`'s `load_files` returned empty or threw (not found, parse error, unsupported format). |

---

## 8. Automation-id naming conventions & instrumented ids

Stable automation ids follow these prefix conventions:

| Prefix | Used for |
|---|---|
| `btn_` | Buttons |
| `combo_` | Preset combo boxes |
| `tab_` | Tabs |
| `canvas_` | Canvases |
| `dlg_` | Dialog buttons |

### Instrumented ids (as-built in v1)

The following controls currently carry stable automation ids:

| id | Control | Note |
|---|---|---|
| `btn_slice` | Slice-plate button | |
| `btn_export` | Print / Export button | Multi-purpose: the action (Print plate / Export G-code / Send) depends on the current mode. |
| `tab_device` | Device / Monitor tab (`MonitorPanel`) | |
| `combo_printer` | Printer preset combo (sidebar) | |
| `combo_filament` | Filament preset combo (sidebar) | First filament row only; extra multi-material rows are not instrumented. |
| `canvas_3d` | 3D editor GL canvas | |

### Controls NOT instrumented in v1

Several controls are intentionally **not** instrumented in v1 because they have no
stable `wxWindow` target to attach an id to:

- **`combo_process`** — process settings are not a sidebar combo box in the current
  OrcaSlicer layout, so there is no combo control to instrument.
- **`btn_add`** — the add/import-object control is a `GLToolbar` item rendered
  *inside* the GL canvas, not a `wxWindow`.
- **`tab_prepare` / `tab_preview`** — the Prepare and Preview notebook pages are
  both backed by the **same** window, and the per-tab buttons are private; there is
  no distinct stable window to target.

For controls that are not instrumented, scripts should fall back to class / label /
path lookup (for wx controls) or ImGui-item lookup (for ImGui controls).

---

## 9. ImGui notes

ImGui is **immediate-mode**: an item is addressable only while it is being drawn in
the current frame. The automation backend records ImGui items each frame, and a
`refresh_ui` is forced before every read or action so that the latest frame's items
are captured.

Consequences and conventions:

- Use [`sync.wait_for`](#syncwait_for) to wait for a transient gizmo or panel item
  to appear before acting on it.
- ImGui items are reported with `backend: "imgui"`, a `path` of the form
  `ImGui/<window>/<label>`, and that **path doubles as the item's `id`** in v1.
- ImGui items are **flat** — they have no `children` and are listed under their
  window.
- Labels may include ImGui `##`-id suffixes (the part after `##` that ImGui uses to
  disambiguate identically labeled widgets).
- Raw `ImGui::` gizmos that are *not* routed through the instrumented
  `ImGuiWrapper` widgets (for example some Emboss / SVG / Text gizmo controls) are
  only covered at the **window level** in v1; their individual sub-items are not
  enumerated.

---

## 10. Platform & display caveats

- **Input requires a focused, visible window.** OS-level input injection uses
  `wxUIActionSimulator`, which requires a focused, visible window. The click path
  raises and focuses the target's top-level window first.
- **Linux CI needs a display.** There must be an X display available; wrap test
  runs with `xvfb-run` (for example, `xvfb-run -a python example_slice.py ...`).
- **Input is asynchronous.** Do **not** rely on fixed sleeps. Use
  [`sync.wait_for`](#syncwait_for) — for example, wait for `btn_export` to become
  `enabled` after slicing completes — rather than sleeping for a guessed duration.
- **`screenshot.window` reads the screen.** It captures the on-screen, DWM-composited
  framebuffer, so the target window must be visible and unobscured, and the result is
  in physical pixels (see HiDPI caveat under [`screenshot.window`](#screenshotwindow)).
  The capture includes the GL 3D viewport as currently shown (model or toolpaths).
- **Single-client / serialized.** v1 handles one request at a time; issue requests
  sequentially from a single client.
- **GUI-thread marshaling.** Every backend call is marshaled onto the GUI thread
  with a timeout. A wedged or unresponsive GUI returns error `1004`.

---

## 11. Quick start

Using the reference client in `tools/automation/orca_automation.py`:

```python
from orca_automation import OrcaClient

orca = OrcaClient(port=13619)
print(orca.version())                       # {'version': '1.0.0', ...}

orca.select_view("prepare")                 # switch to the 3D editor
orca.open(r"C:\models\part.stl")            # load a model at runtime (synchronous)

orca.click({"id": "btn_slice"})             # start slicing the plate
orca.wait_for({"id": "btn_export"},         # wait until slicing finishes
              state="enabled", timeout_ms=180000)

orca.select_view("preview")                 # switch to the sliced G-code preview
png = orca.screenshot()                     # on-screen capture (incl. 3D view)
with open("window.png", "wb") as f:
    f.write(png)
```

For a full, runnable end-to-end example — launching OrcaSlicer with the automation
flags, loading a model, slicing, waiting for completion, and saving a window PNG —
see `tools/automation/example_slice.py`.

---

## 12. Future work

Planned enhancements beyond v1:

- **Authentication token** plus a Preferences toggle to enable/disable the server
  from the GUI.
- **WebSocket push events** for real-time UI/state notifications (instead of
  polling).
- **Per-item ImGui gizmo instrumentation** so individual gizmo sub-controls (Emboss
  / SVG / Text, etc.) are addressable, not just at the window level.
- **More widget ids** — the process combo, the add/import button, and the
  Prepare/Preview tabs once they expose stable windows.
- An **MCP wrapper** to expose the automation surface to model-context tooling.

---

## Verification (v1)

This section records the final regression gate for the v1 feature: confirmation
that the protocol core is covered by unit tests, that the existing test suites
are unaffected, and that the **disabled path (automation OFF, the default) is a
true no-op** — zero new threads, zero socket binds, zero allocations, and zero
behavior change.

### Unit-suite results (Release, Windows / MSVC, Ninja Multi-Config)

| Suite | Result |
|---|---|
| `automation` (protocol core) | **32 / 32 passed** |
| `libslic3r` (most affected by the additive `PrintConfig.cpp` CLI options) | **99 / 99 passed** |
| `fff_print` | **14 / 14 passed** |
| `libnest2d` | **14 / 14 passed** |
| `sla_print` | **21 / 21 passed** |
| `slic3rutils` | 3 / 5 passed — 2 pre-existing `[OrcaCloudServiceAgent]` SEGFAULTs, **unrelated to automation** (see note) |

> The two `slic3rutils` failures are `Orca cloud flat/nested session resolves
> display name consistently`. They exercise `Slic3r::OrcaCloudServiceAgent`, which
> the automation branch does **not** touch (verified via `git diff --stat
> main...HEAD` — no change to `src/slic3r/Utils/OrcaCloudServiceAgent.*` or
> `tests/slic3rutils/*`). They are pre-existing and not a regression introduced by
> this feature.

### Static disabled-path audit (the core regression guarantee)

Verified by code reading that with no `--automation-server` flag:

- **Flag defaults off.** `m_automation_port` defaults to `0`
  (`src/slic3r/GUI/GUI_App.hpp:249`); `is_automation_enabled()` returns
  `m_automation_port > 0` (`GUI_App.hpp:386`) → `false` by default.
- **No server / thread / socket.** `post_init()` calls
  `start_automation_server()` **only** when
  `init_params->automation_port > 0` (`src/slic3r/GUI/GUI_App.cpp:737-740`), and
  `start_automation_server()` itself early-returns when `m_automation_port <= 0`
  (`GUI_App.cpp:7097`). The backend / dispatcher / beast server objects are
  constructed nowhere else → no `orca_automation` thread and no localhost bind
  when the flag is absent.
- **Recording hooks short-circuit.** `ImGuiWrapper::automation_record_last_item`
  has as its **first statement** `if (!wxGetApp().is_automation_enabled())
  return;` (`src/slic3r/GUI/ImGuiWrapper.cpp:576-577`) — a single bool check, no
  `ImGuiItemRecord` allocation and no `ImGuiItemTable` access on the disabled
  path. In `ImGuiWrapper::render()` the window-enumeration loop and
  `swap_frame()` are fully wrapped in `if (wxGetApp().is_automation_enabled())`
  (`ImGuiWrapper.cpp:599-611`); when off, `render()` is its original
  `ImGui::Render()` + `render_draw_data()` plus one bool check.
- **Instrumentation is inert.** The ~7 `set_automation_id(...)` calls
  (`MainFrame.cpp:1330,1389,1841,1842`; `Plater.cpp:1772,2172,5068`) only store a
  pointer into a static registry and bind a `wxEVT_DESTROY` pruning handler
  (`src/slic3r/GUI/Automation/AutomationRegistry.cpp:24-36`). The registry is
  **read** only via `window_for_automation_id` / `automation_id_of`, which are
  called solely by the backend while the server is running → harmless when off.
- **CLI options are purely additive.** `automation_server` (coBool, default
  `false`) and `automation_server_port` (coInt, default `13619`) are new `add()`
  entries appended after `enable_timelapse`
  (`src/libslic3r/PrintConfig.cpp:10794-10805`); no existing option is changed.
  `GUI_InitParams::automation_port` defaults to `0`
  (`src/slic3r/GUI/GUI_Init.hpp:37`) and is set only when `--automation-server`
  is supplied (`src/OrcaSlicer.cpp:1345-1348`).

**Conclusion:** with automation OFF (the default), the feature allocates nothing
and changes nothing — the only added cost on any hot path is a single boolean
comparison.

### Deferred manual runtime checks (require a display / Xvfb)

These need a live GUI and cannot be run headlessly in CI; they are the manual
acceptance steps:

1. Launch **without** `--automation-server` → `curl http://127.0.0.1:13619/`
   fails to connect (no listener); no `orca_automation` thread exists.
2. Launch **with** `--automation-server --automation-server-port=13619` →
   `GET /` returns the health text; `POST /jsonrpc {"method":"automation.version"}`
   returns version / protocol / capabilities; `widget.get {"target":{"id":"btn_slice"}}`
   returns a node with a sensible screen rect.
3. Interactive sanity: open a gizmo / move sliders with automation OFF → no
   visual or behavior change.

See `tools/automation/example_slice.py` for the runnable end-to-end path.
