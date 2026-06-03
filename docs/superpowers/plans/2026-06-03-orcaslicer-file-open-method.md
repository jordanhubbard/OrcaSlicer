# `file.open` Automation Method Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `file.open` JSON-RPC automation method that loads one or more files into an already-running OrcaSlicer instance by calling `Plater::load_files(...)` synchronously on the GUI thread.

**Architecture:** Follows the existing `screenshot.window` / `app.state` method pattern. A new pure-virtual `open_files(paths)` is added to the wx-free `IUiBackend` interface; `WxUiBackend` implements it via the existing `run_on_gui(...)` GUI-thread marshal calling `Plater::load_files`; the `JsonRpcDispatcher` gains a `file.open` route, a param-parsing helper, and a new `kErrLoadFailed = 1007` error code. The unit-testable surface (dispatcher + param validation + routing) is driven against `MockUiBackend`.

**Tech Stack:** C++17, nlohmann::json, Catch2 v2 (`catch_all.hpp` / `Catch2WithMain`), wxWidgets, CMake + Ninja Multi-Config. Python 3 reference client (stdlib only).

---

## Design-spec note (resolve before coding)

The design spec's error table reads `1002 | kInvalidParams | paths missing/empty…`, but in the codebase `kInvalidParams` is the standard JSON-RPC code **`-32602`**, while `1002` is `kErrNotActionable`. The spec's **Constant column (`kInvalidParams`) is authoritative** and matches every other param-validation path in the dispatcher (e.g. `m_input_type` throws `kInvalidParams` for a bad `text`). This plan therefore validates `file.open` params with **`kInvalidParams` (-32602)**, exactly like the existing handlers, and the tests assert `== kInvalidParams`. The literal "1002" in the spec is a typo; do not emit code 1002 for param errors.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/slic3r/GUI/Automation/IUiBackend.hpp` | modify | Add pure-virtual `int open_files(paths)` to the backend abstraction (stays wx-free). |
| `src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp` | modify | Add `kErrLoadFailed = 1007` constant + `m_file_open` declaration. |
| `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp` | modify | Add `parse_paths` helper, `m_file_open` body, dispatch route, capabilities entry. |
| `src/slic3r/GUI/Automation/WxUiBackend.hpp` | modify | Declare `open_files` override. |
| `src/slic3r/GUI/Automation/WxUiBackend.cpp` | modify | Implement `open_files` via `run_on_gui` → `Plater::load_files`. |
| `tests/automation/MockUiBackend.hpp` | modify | `open_files` override: record paths + return-count + fail knob. |
| `tests/automation/test_dispatcher.cpp` | modify | Catch2 tests for routing, string/array, validation, failure, capabilities. |
| `tools/automation/orca_automation.py` | modify | `open(self, paths)` client wrapper. |
| `tools/automation/example_slice.py` | modify | Launch without a model arg, then `orca.open([model])`. |
| `doc/automation.md` | modify | Document the method, capabilities, error `1007`. |

**Build/test layout:** Ninja Multi-Config in `build/`. The unit suite target is `automation_tests`; its sources (`tests/automation/CMakeLists.txt`) compile `JsonRpcDispatcher.cpp` + `MockUiBackend` but **not** `WxUiBackend.cpp`. So dispatcher/mock changes are fully unit-testable headlessly; `WxUiBackend.cpp` is verified by the full app build only.

---

## Task 1: Extend the backend abstraction (interface + mock + error code)

Adds the `open_files` contract so tests can be written. Adding a pure virtual to `IUiBackend` forces every implementation to provide it — in the unit-test target that is only `MockUiBackend`, so this task keeps the `automation_tests` build green.

**Files:**
- Modify: `src/slic3r/GUI/Automation/IUiBackend.hpp`
- Modify: `src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp:19`
- Modify: `tests/automation/MockUiBackend.hpp`

- [ ] **Step 1: Add the error constant**

In `src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp`, after the existing `kErrDisabled` line (currently line 19), add:

```cpp
constexpr int kErrDisabled        = 1006;
constexpr int kErrLoadFailed      = 1007; // file.open: load_files returned empty / threw
```

- [ ] **Step 2: Add the pure-virtual `open_files` to the interface**

In `src/slic3r/GUI/Automation/IUiBackend.hpp`, inside `class IUiBackend`, immediately after the `screenshot_window` pure virtual (currently line 97), add:

```cpp
    // Load one or more files (absolute paths) into the running instance on the GUI
    // thread. Returns the number of objects added to the scene (load_files(...).size()).
    // Throws AutomationError(kErrLoadFailed) when nothing loads. Header stays wx-free:
    // the concrete LoadStrategy is chosen inside WxUiBackend, not exposed here.
    virtual int open_files(const std::vector<std::string>& paths) = 0;
```

- [ ] **Step 3: Implement `open_files` in the mock with record + knobs**

In `tests/automation/MockUiBackend.hpp`: add an include for the error constant near the top (after the `IUiBackend.hpp` include on line 2):

```cpp
#include "slic3r/GUI/Automation/IUiBackend.hpp"
#include "slic3r/GUI/Automation/JsonRpcDispatcher.hpp" // kErrLoadFailed
```

Add recorded-call + canned-output members. After the `screenshot_window_count` recorded field (line 20) add:

```cpp
    int               screenshot_window_count   = 0;
    std::vector<std::vector<std::string>> opened_paths; // paths of each open_files()
```

After the `click_result` canned field (line 26) add:

```cpp
    bool     click_result = true;
    int      open_return_count = 0;     // value open_files() returns
    bool     open_should_fail = false;  // when true, open_files() throws kErrLoadFailed
```

Add the override next to the other overrides, after `screenshot_window` (lines 49-51):

```cpp
    PngImage screenshot_window(const UiNode*) override {
        ++screenshot_window_count; return canned_png;
    }
    int      open_files(const std::vector<std::string>& paths) override {
        opened_paths.push_back(paths);
        if (open_should_fail)
            throw AutomationError(kErrLoadFailed, "mock load failed");
        return open_return_count;
    }
```

- [ ] **Step 4: Build the unit-test target to confirm it still compiles**

Run: `cmake --build build --config RelWithDebInfo --target automation_tests`
Expected: build succeeds (the new pure virtual is satisfied by the mock; no behavior change yet).

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/IUiBackend.hpp src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp tests/automation/MockUiBackend.hpp
git commit -m "feat(automation): add open_files to backend interface + kErrLoadFailed (1007)"
```

---

## Task 2: `file.open` dispatcher handler (parse, route, validate, fail)

Implements the full JSON-RPC handler against the mock: param parsing (string or array), validation, routing to `open_files`, and `kErrLoadFailed` propagation.

**Files:**
- Modify: `src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp:49` (declaration)
- Modify: `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`
- Test: `tests/automation/test_dispatcher.cpp`

- [ ] **Step 1: Write the failing happy-path test (array of paths)**

Append to `tests/automation/test_dispatcher.cpp`:

```cpp
TEST_CASE("file.open with an array of paths routes to backend", "[automation][rpc]") {
    MockUiBackend mock;
    mock.open_return_count = 3;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",1},{"method","file.open"},
        {"params",{{"paths", json::array({"C:/abs/a.stl","C:/abs/b.stl"})}}}});
    CHECK(resp.at("result").at("ok") == true);
    CHECK(resp.at("result").at("loaded") == 3);
    REQUIRE(mock.opened_paths.size() == 1);
    REQUIRE(mock.opened_paths[0].size() == 2);
    CHECK(mock.opened_paths[0][0] == "C:/abs/a.stl");
    CHECK(mock.opened_paths[0][1] == "C:/abs/b.stl");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config RelWithDebInfo --target automation_tests && build/tests/automation/RelWithDebInfo/automation_tests.exe "file.open with an array of paths routes to backend"`
Expected: FAIL — `file.open` is an unknown method, so the response carries `error.code == -32601` and has no `result` (the `resp.at("result")` access throws). (If the exe path differs on your machine, locate it with `find build -iname automation_tests.exe`.)

- [ ] **Step 3: Declare the handler**

In `src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp`, after the `m_screenshot_window` declaration (currently line 49) add:

```cpp
    nlohmann::json m_screenshot_window(const nlohmann::json& params);
    nlohmann::json m_file_open(const nlohmann::json& params);
```

- [ ] **Step 4: Add the `parse_paths` helper and `m_file_open` body**

In `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`, add a `parse_paths` helper. Place it in the anonymous namespace that also holds `parse_keys` — insert it right before that namespace's closing `} // namespace` (currently line 130):

```cpp
// "paths" may be a single string ("C:/a.stl") or an array of strings. Returns the
// non-empty absolute paths; throws kInvalidParams when paths is missing, not a
// string/array, contains a non-string entry, or yields no non-empty path.
std::vector<std::string> parse_paths(const nlohmann::json& params) {
    if (!params.is_object() || !params.contains("paths"))
        throw AutomationError(kInvalidParams, "file.open requires 'paths'");
    const auto& p = params.at("paths");
    std::vector<std::string> out;
    if (p.is_string()) {
        out.push_back(p.get<std::string>());
    } else if (p.is_array()) {
        for (const auto& e : p) {
            if (!e.is_string())
                throw AutomationError(kInvalidParams, "'paths' entries must be strings");
            out.push_back(e.get<std::string>());
        }
    } else {
        throw AutomationError(kInvalidParams, "'paths' must be a string or array");
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::string& s) { return s.empty(); }),
              out.end());
    if (out.empty())
        throw AutomationError(kInvalidParams, "'paths' is empty");
    return out;
}
```

(`<algorithm>` for `std::remove_if` is already included at the top of the file, line 4.)

Add the handler body next to the other handlers. After `m_screenshot_window` (currently ends line 343, just before the final `}}} // namespace`), add:

```cpp
nlohmann::json JsonRpcDispatcher::m_file_open(const nlohmann::json& params) {
    const std::vector<std::string> paths = parse_paths(params);
    const int loaded = m_backend.open_files(paths);
    return { {"ok", true}, {"loaded", loaded} };
}
```

- [ ] **Step 5: Add the dispatch route**

In `JsonRpcDispatcher::dispatch`, after the `screenshot.window` route (currently line 195) add:

```cpp
        if (method == "screenshot.window")         return make_result(id, m_screenshot_window(params));
        if (method == "file.open")                 return make_result(id, m_file_open(params));
```

- [ ] **Step 6: Run the happy-path test to verify it passes**

Run: `cmake --build build --config RelWithDebInfo --target automation_tests && build/tests/automation/RelWithDebInfo/automation_tests.exe "file.open with an array of paths routes to backend"`
Expected: PASS — 1 test case, all assertions passed.

- [ ] **Step 7: Add the remaining handler tests (string, validation, failure)**

Append to `tests/automation/test_dispatcher.cpp`:

```cpp
TEST_CASE("file.open accepts a bare string path", "[automation][rpc]") {
    MockUiBackend mock;
    mock.open_return_count = 1;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",2},{"method","file.open"},
        {"params",{{"paths","C:/abs/a.stl"}}}});
    CHECK(resp.at("result").at("loaded") == 1);
    REQUIRE(mock.opened_paths.size() == 1);
    REQUIRE(mock.opened_paths[0].size() == 1);
    CHECK(mock.opened_paths[0][0] == "C:/abs/a.stl");
}

TEST_CASE("file.open with missing paths -> invalid params", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",3},{"method","file.open"},
        {"params", json::object()}});
    CHECK(resp.at("error").at("code") == kInvalidParams);
    CHECK(mock.opened_paths.empty());
}

TEST_CASE("file.open with empty paths array -> invalid params", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",4},{"method","file.open"},
        {"params",{{"paths", json::array()}}}});
    CHECK(resp.at("error").at("code") == kInvalidParams);
    CHECK(mock.opened_paths.empty());
}

TEST_CASE("file.open with a non-string entry -> invalid params", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",5},{"method","file.open"},
        {"params",{{"paths", json::array({"C:/a.stl", 42})}}}});
    CHECK(resp.at("error").at("code") == kInvalidParams);
    CHECK(mock.opened_paths.empty());
}

TEST_CASE("file.open backend load failure -> 1007", "[automation][rpc]") {
    MockUiBackend mock;
    mock.open_should_fail = true;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",6},{"method","file.open"},
        {"params",{{"paths","C:/abs/a.stl"}}}});
    CHECK(resp.at("error").at("code") == kErrLoadFailed);
}
```

- [ ] **Step 8: Run all file.open tests to verify they pass**

Run: `cmake --build build --config RelWithDebInfo --target automation_tests && build/tests/automation/RelWithDebInfo/automation_tests.exe "file.open*"`
Expected: PASS — 6 test cases, all assertions passed.

- [ ] **Step 9: Commit**

```bash
git add src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp tests/automation/test_dispatcher.cpp
git commit -m "feat(automation): add file.open dispatcher handler with validation + tests"
```

---

## Task 3: Advertise `file.open` in `automation.version` capabilities

**Files:**
- Modify: `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp:166-172`
- Test: `tests/automation/test_dispatcher.cpp`

- [ ] **Step 1: Write the failing capabilities test**

Append to `tests/automation/test_dispatcher.cpp`:

```cpp
TEST_CASE("automation.version capabilities include file.open", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",1},{"method","automation.version"}});
    const auto& caps = resp.at("result").at("capabilities");
    bool found = false;
    for (const auto& c : caps) if (c == "file.open") found = true;
    CHECK(found);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config RelWithDebInfo --target automation_tests && build/tests/automation/RelWithDebInfo/automation_tests.exe "automation.version capabilities include file.open"`
Expected: FAIL — `CHECK(found)` is false; `file.open` is not yet in the capabilities array.

- [ ] **Step 3: Add `file.open` to the capabilities array**

In `JsonRpcDispatcher::m_version` (currently lines 166-172), add `"file.open"` to the array:

```cpp
nlohmann::json JsonRpcDispatcher::m_version(const nlohmann::json&) {
    return { {"version", kAutomationVersion},
             {"protocol", "2.0"},
             {"capabilities", nlohmann::json::array({
                 "tree.dump","tree.find","widget.get","input.click","input.type",
                 "input.key","sync.wait_for","app.state","screenshot.window",
                 "file.open" })} };
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --config RelWithDebInfo --target automation_tests && build/tests/automation/RelWithDebInfo/automation_tests.exe "automation.version capabilities include file.open"`
Expected: PASS.

- [ ] **Step 5: Run the whole automation suite to confirm no regressions**

Run: `cmake --build build --config RelWithDebInfo --target automation_tests && build/tests/automation/RelWithDebInfo/automation_tests.exe --order rand --warn NoAssertions`
Expected: PASS — all cases green (the pre-existing ~32 plus the 7 new `file.open`/capabilities cases ≈ 39).

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp tests/automation/test_dispatcher.cpp
git commit -m "feat(automation): advertise file.open in automation.version capabilities"
```

---

## Task 4: Implement `WxUiBackend::open_files` (real GUI-thread load)

Not covered by the headless unit suite (`WxUiBackend.cpp` is excluded from `automation_tests`); verified by the full app build + the manual runtime check in Task 8.

**Files:**
- Modify: `src/slic3r/GUI/Automation/WxUiBackend.hpp:21`
- Modify: `src/slic3r/GUI/Automation/WxUiBackend.cpp`

- [ ] **Step 1: Declare the override**

In `src/slic3r/GUI/Automation/WxUiBackend.hpp`, after the `screenshot_window` declaration (line 21) add:

```cpp
    PngImage screenshot_window(const UiNode* target) override;
    int      open_files(const std::vector<std::string>& paths) override;
```

- [ ] **Step 2: Implement `open_files`**

In `src/slic3r/GUI/Automation/WxUiBackend.cpp`, add the implementation just before the final `}}} // namespace Slic3r::GUI::Automation` (currently line 306):

```cpp
int WxUiBackend::open_files(const std::vector<std::string>& paths) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> int {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            throw AutomationError(kErrLoadFailed, "no plater to load into");
        // Default strategy matches drag-drop / Plater::load_files's own default: it
        // routes .3mf as a project and meshes as models based on file content, so no
        // as_project flag is needed in v1. ask_multi=false: never prompt.
        const LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig;
        std::vector<size_t> loaded;
        try {
            loaded = plater->load_files(paths, strategy, /*ask_multi=*/false);
        } catch (const std::exception& e) {
            throw AutomationError(kErrLoadFailed,
                                  std::string("load_files failed: ") + e.what());
        }
        if (loaded.empty())
            throw AutomationError(kErrLoadFailed, "load_files loaded nothing");
        return static_cast<int>(loaded.size());
    });
}
```

Notes for the implementer:
- `LoadStrategy` and its `operator|` (namespace `Slic3r`, from `libslic3r/Format/bbs_3mf.hpp`) are already in scope: `WxUiBackend.cpp` includes `Plater.hpp` (line 7), which transitively pulls in the enum, and this translation unit lives in `Slic3r::GUI::Automation` so unqualified `LoadStrategy` resolves via the enclosing `Slic3r` namespace. No new include is required.
- `Plater::load_files(const std::vector<std::string>&, LoadStrategy, bool)` is the existing string overload (`Plater.hpp:379`) — no `boost::filesystem::path` conversion needed.
- `kErrLoadFailed` comes from `JsonRpcDispatcher.hpp`, already included at line 4.
- An `AutomationError` thrown inside the `run_on_gui` lambda is captured by the helper's `set_exception` and rethrown from `fut.get()`, so the 1007 code propagates to the dispatcher unchanged.

- [ ] **Step 3: Build the full app to verify it compiles and links**

Run: `cmake --build build --config RelWithDebInfo --target OrcaSlicer`
Expected: build succeeds (no missing-symbol / pure-virtual errors; `WxUiBackend` is now concrete).

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/Automation/WxUiBackend.hpp src/slic3r/GUI/Automation/WxUiBackend.cpp
git commit -m "feat(automation): implement WxUiBackend::open_files via Plater::load_files"
```

---

## Task 5: Python client wrapper `OrcaClient.open`

**Files:**
- Modify: `tools/automation/orca_automation.py:80-82`

- [ ] **Step 1: Add the `open` method**

In `tools/automation/orca_automation.py`, after the `key` method (ends line 82), add:

```python
    def key(self, keys) -> dict:
        # keys: "ctrl+s" or ["ctrl", "s"]
        return self._call("input.key", {"keys": keys})

    def open(self, paths) -> dict:
        """Load one or more files into the running instance at runtime.

        `paths` is a single absolute path string or a list of them. Paths are read
        from the host filesystem by the server (localhost-only). Returns
        {"ok": True, "loaded": <count>}. Raises OrcaError 1007 on load failure."""
        if isinstance(paths, str):
            paths = [paths]
        return self._call("file.open", {"paths": list(paths)})
```

- [ ] **Step 2: Smoke-test the wrapper's normalization offline (no server needed)**

Run:
```bash
python -c "import sys; sys.path.insert(0, 'tools/automation'); import orca_automation as m; c = m.OrcaClient.__new__(m.OrcaClient); c._call = lambda meth, params=None: (meth, params); print(c.open('C:/a.stl')); print(c.open(['C:/a.stl','C:/b.stl']))"
```
Expected output:
```
('file.open', {'paths': ['C:/a.stl']})
('file.open', {'paths': ['C:/a.stl', 'C:/b.stl']})
```

- [ ] **Step 3: Commit**

```bash
git add tools/automation/orca_automation.py
git commit -m "feat(automation): add OrcaClient.open() wrapper for file.open"
```

---

## Task 6: Update `example_slice.py` to load at runtime via `file.open`

**Files:**
- Modify: `tools/automation/example_slice.py:26-52`

- [ ] **Step 1: Launch without the model arg, then call `open`**

In `tools/automation/example_slice.py`, change the `subprocess.Popen` call (lines 26-31) to drop the trailing model positional:

```python
    proc = subprocess.Popen([
        args.orca,
        "--automation-server",
        f"--automation-server-port={args.port}",
    ])
```

Then replace the project-load wait block (currently lines 46-51) so the model is loaded at runtime via `file.open` instead of relying on a launch-time positional:

```python
        # Load the model into the already-running instance, then wait until the
        # project reports loaded. file.open is synchronous, so project_loaded is
        # already true on return; the wait is a belt-and-suspenders guard.
        orca.open([args.model])
        deadline = time.time() + 30
        while time.time() < deadline:
            if orca.app_state().get("project_loaded"):
                break
            time.sleep(0.5)
```

- [ ] **Step 2: Byte-compile the script to confirm no syntax errors**

Run: `python -m py_compile tools/automation/example_slice.py`
Expected: no output, exit code 0.

- [ ] **Step 3: Commit**

```bash
git add tools/automation/example_slice.py
git commit -m "docs(automation): example_slice.py loads model at runtime via file.open"
```

---

## Task 7: Document `file.open` in `doc/automation.md`

**Files:**
- Modify: `doc/automation.md` (capabilities example §4 line 111-114; new method subsection after `screenshot.window`; error table §7)

- [ ] **Step 1: Add `file.open` to the capabilities example**

In `doc/automation.md`, update the `automation.version` result example (lines 111-114) to include `file.open`:

```json
  "capabilities": [
    "tree.dump", "tree.find", "widget.get", "input.click", "input.type",
    "input.key", "sync.wait_for", "app.state", "screenshot.window", "file.open"
  ]
```

The §4 prose count is already written for this: "There are 11 methods … the 10 callable feature methods" now matches exactly (10 capability entries + `automation.version` = 11). Leave that sentence unchanged.

- [ ] **Step 2: Add the `file.open` method subsection**

In `doc/automation.md`, immediately after the `screenshot.window` method subsection (it ends just before the `---` on line 303) and before that `---`, insert:

```markdown
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
```

- [ ] **Step 3: Add the `1007` row to the error-code table**

In `doc/automation.md` §7, in the application-specific codes table, after the `1006` row (line 395) add:

```markdown
| `1006` | Disabled. |
| `1007` | Load failed — `file.open`'s `load_files` returned empty or threw (not found, parse error, unsupported format). |
```

- [ ] **Step 4: Commit**

```bash
git add doc/automation.md
git commit -m "docs(automation): document file.open method and error 1007"
```

---

## Final verification

- [ ] **Step 1: Full automation unit suite green**

Run: `cmake --build build --config RelWithDebInfo --target automation_tests && build/tests/automation/RelWithDebInfo/automation_tests.exe --order rand --warn NoAssertions`
Expected: PASS — all cases (pre-existing ~32 + 7 new) green, no `NoAssertions` warnings on the new cases.

- [ ] **Step 2: Full app builds**

Run: `cmake --build build --config RelWithDebInfo --target ALL_BUILD -- -m`
Expected: build succeeds.

- [ ] **Step 3: Manual runtime check (requires a display)**

Launch with `--automation-server` and **no** model arg, then from a Python shell:
```python
from orca_automation import OrcaClient
orca = OrcaClient(port=13619)
print(orca.open(["C:/abs/path/cube.stl"]))   # -> {'ok': True, 'loaded': 1}
print(orca.app_state()["project_loaded"])    # -> True
open("window.png","wb").write(orca.screenshot())  # PNG shows the loaded model
```
Expected: `loaded >= 1`, `project_loaded == True`, screenshot shows the model.

- [ ] **Step 4: Gating check (automation OFF is a no-op)**

Confirm by reading: with no `--automation-server` flag, the server/backend/dispatcher are never constructed (`GUI_App.cpp` `start_automation_server()` early-return), so `file.open` is unreachable. No new hot-path cost beyond the existing single bool check. (See `doc/automation.md` §Verification — disabled-path audit; this feature adds no new gating surface.)

---

## Backward compatibility

Additive only: one new method (`file.open`), one new error code (`1007`), one new capabilities entry, and one new backend interface method. No existing method, profile, project-file handling, or default behavior changes. The method is reachable only when `--automation-server` is passed.
