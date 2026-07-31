# AI Slicer — architecture

This document describes the design of the optional AI assistant in OrcaSlicer as
it exists in the tree. It is the companion to [README.md](README.md) (the user
guide) and [UPSTREAM-PR.md](UPSTREAM-PR.md) (the upstreaming proposal).

Everything here is opt-in: with no provider configured, no AI code path makes a
network call, and the only visible trace is an inert prompt box in the sidebar.

---

## 1. Goals and non-goals

**Goals**

- Let a user describe a *mechanical* part in plain language and get printable
  geometry on the plate.
- Ground that generation in the machine that will actually print it — bed size,
  nozzle, material, process settings, and (when a Physical Printer is
  configured) live telemetry and a camera frame.
- Stay provider-agnostic: OpenAI, Anthropic, and any OpenAI-compatible gateway
  (Ollama, LiteLLM, vLLM, an Azure/OpenAI proxy, …).
- Add **no new third-party dependencies**. HTTP reuses `Slic3r::Http`, JSON
  reuses the vendored `nlohmann::json`, geometry reuses `libslic3r`.

**Non-goals**

- Organic or sculptural modelling. The generator emits CSG over primitives; it
  cannot produce animals, faces, or freeform surfaces. Those are handled by
  pointing the user at community model sites instead.
- Cloud accounts, telemetry, or any Orca-operated service. The user brings their
  own endpoint and key.
- Slicing decisions. Nothing in this feature alters the slicing pipeline,
  profiles, or project-file format.

---

## 2. Component map

| Layer | Files | Role |
|---|---|---|
| **Provider abstraction** | `src/slic3r/Utils/AIProvider.{hpp,cpp}` | `AIProvider` interface + `AIProvider::create()` factory. Concrete `OpenAIProvider` and `AnthropicProvider` live in the anonymous namespace of the `.cpp` and are reachable only through the factory. |
| **Machine context** | `src/slic3r/Utils/AIPrinterContext.{hpp,cpp}` | Builds `AIPrinterContext`: slicer settings (always), live host telemetry (optional), camera JPEG (optional). Renders to prompt text / base64 image. |
| **Text-to-shape core** | `src/slic3r/Utils/AIShapeGen.{hpp,cpp}` | Pure `libslic3r`. Validates a JSON *shape spec*, bakes it into a `Model`, repairs it to a manifold, and checks it against the build volume. No GUI, no network. |
| **Generation pipeline** | `src/slic3r/GUI/AIGenerate.hpp` (declaration), implemented in `src/slic3r/GUI/AISlicerDialog.cpp` | `ai_generate_shape_to_plate()` — the single entry point shared by every UI surface. Owns the background `Job` and the repair loop. |
| **Dialog UI** | `src/slic3r/GUI/AISlicerDialog.{hpp,cpp}` | Two-tab dialog: *Generate shape* and *Find models*. |
| **Sidebar UI** | `src/slic3r/GUI/Plater.cpp` (`Sidebar` construction) | Compact prompt + **Generate** button at the top of the parameter panel. |
| **Settings UI** | `src/slic3r/GUI/Preferences.cpp` (*General ▸ AI Slicer*) | Provider / gateway / key / model, plus **Test connection** and **Qualify for 3D**. Built from the page's standard row builders. |
| **Menu** | `src/slic3r/GUI/MainFrame.cpp` | Top-level **AI** menu with *Generate 3D Shape…*. |

All sources are registered in `src/slic3r/CMakeLists.txt` and link into the
existing `libslic3r_gui` target; there is no new build target and no new
find_package.

### Layering

```
              Preferences (General ▸ AI Slicer)      MainFrame (AI menu)
                          │                                  │
                          │ writes ai_slicer/*               │ opens
                          ▼                                  ▼
   Plater sidebar ──┐                                 AISlicerDialog
                    │                                        │
                    └──────────► ai_generate_shape_to_plate() ◄┘
                                          │  (AIGenerate.hpp)
              ┌───────────────────────────┼───────────────────────────┐
              ▼                           ▼                           ▼
      AIPrinterContext              AIProvider                  AIShapeGen
      (GUI + network)          (network, provider-specific)   (pure libslic3r)
```

`AIShapeGen` and `AIPrinterContext::slicer_context_from_config()` are the two
GUI-free, network-free pieces — deliberately, so they can be unit-tested and so
the geometry code has no wx dependency.

---

## 3. `AIProvider` — the provider abstraction

```cpp
struct AIConfig  { std::string provider, api_key, model, base_url; };
struct AIMessage { std::string role, content; std::vector<std::string> images; };
struct AIResponse{ bool ok; std::string content, error;
                   nlohmann::json raw;
                   std::string tool_call_arguments, tool_call_name; };

class AIProvider {
    virtual const char *get_name() const = 0;
    virtual AIResponse  chat(const std::vector<AIMessage>&, const nlohmann::json &params = {}) const = 0;
    virtual AIResponse  complete(const std::string &prompt, const nlohmann::json &params = {}) const;  // default: wraps in a user message
    virtual bool        test_connection(std::string &error_msg) const = 0;
    virtual bool        get_models(std::vector<AIModelInfo>&, std::string &error_msg) const = 0;

    static AIProvider *create(const AIConfig&);
    static AIConfig    config_from_app_config(const AppConfig&);
};
```

The shape deliberately mirrors `PrintHost::get_print_host()`: callers hold a
`std::unique_ptr<AIProvider>` obtained from the factory and never name a
concrete class.

**Design points**

- `chat()` is synchronous. All call sites run it on a background `Job` thread
  (see §5), so the blocking round-trip never touches the UI thread.
- `params` is a free-form JSON object merged into the request body, which is how
  `tools`, `tool_choice`, `temperature`, and `max_tokens` are passed without the
  interface growing a parameter per provider knob.
- `AIResponse::tool_call_arguments` carries the raw JSON argument string of the
  first tool call. An empty value on a *forced* tool call is the signal that the
  model answered in prose and is therefore unsuitable for this task.
- Errors never throw. Every failure path fills `AIResponse::error` (or the
  `error_msg` out-param) with a message already fit for a status label; HTTP
  401/403/404/429 are mapped to specific hints in `http_error_message()`.

### Implementations

`OpenAIProvider` handles both OpenAI and any OpenAI-compatible gateway (the
factory just supplies a different default base URL and display name):

- `POST {base}/v1/chat/completions`, `GET {base}/v1/models`,
  `Authorization: Bearer <key>`.
- `api_url()` avoids doubling `/v1` when the user pastes a base URL that already
  ends in `/v1` — the common shape for self-hosted gateways.
- Multimodal messages become an array of `{"type":"text"}` /
  `{"type":"image_url"}` parts with a `data:image/jpeg;base64,` URI.

`AnthropicProvider` targets the Messages API:

- `POST {base}/v1/messages`, `GET {base}/v1/models`, `x-api-key` +
  `anthropic-version: 2023-06-01`.
- System turns are hoisted out of `messages` into the top-level `system` field,
  as that API requires; `max_tokens` defaults to 1024 when the caller does not
  set it.
- Multimodal blocks use `{"type":"image","source":{"type":"base64", …}}`.
- The response reader concatenates `text` blocks and captures the first
  `tool_use` block into `tool_call_name` / `tool_call_arguments`.

`test_connection()` is implemented as `get_models()` for both providers: it
proves the endpoint, the key, and the network path in one cheap GET.

### Provider selection

`AIProvider::create()` maps the `provider` key:

| key | result |
|---|---|
| `""`, `none` | `nullptr` — feature disabled |
| `openai` | `OpenAIProvider` against `https://api.openai.com` |
| `anthropic` | `AnthropicProvider` against `https://api.anthropic.com` |
| `compatible`, `openai_compatible` | `OpenAIProvider` against the configured gateway URL; `nullptr` (logged) if no URL is set |
| anything else | `nullptr`, logged as a warning |

Returning `nullptr` is the single "AI is off" signal every caller checks.

---

## 4. `AIPrinterContext` — how the machine reaches the model

`AIPrinterContext::gather()` assembles three independent sources, each of which
degrades silently when unavailable. It never throws.

**1. Slicer settings — always present, works fully offline.**
`slicer_context_from_config(full_config)` reads the merged
`DynamicPrintConfig` and emits three groups:

- `machine`: `nozzle_diameter`, `nozzle_type`, `printable_height`,
  `printable_area`, `gcode_flavor`, `machine_max_speed_{x,y,z,e}`,
  `machine_max_acceleration_extruding`, `machine_max_acceleration_travel`
- `filament`: `filament_type`, `filament_diameter`, `filament_flow_ratio`,
  `nozzle_temperature`, `nozzle_temperature_initial_layer`, `hot_plate_temp`,
  `hot_plate_temp_initial_layer`
- `process`: `layer_height`, `initial_layer_print_height`,
  `sparse_infill_density`

Each value goes through `DynamicPrintConfig::opt_serialize()`, which renders any
option type — scalar, enum, per-extruder vector, point list — to a stable
string. Combined with an existence check before every read, this means the
context never guesses a C++ type and never dereferences a missing option, so it
survives profile changes and unusual printer definitions without a code change.
This function is pure: no GUI, no network, no globals. It is the unit-testable
core of the context.

**2. Live telemetry — only with a selected Physical Printer.**
When `physical_printers.get_selected_printer_config()` yields a host and the
host type is Moonraker or OctoPrint, one `GET` with a 3-second connect timeout
fetches
`/printer/objects/query?extruder&heater_bed&toolhead&print_stats&bed_mesh&gcode_move&probe&configfile`
and stores `result.status` verbatim as `live`, setting `live_available`. Any
failure is logged at info level and leaves `live_available == false`.

**3. Camera frame — best effort.**
The bare hostname is extracted from the print host and probed at the
mjpg-streamer convention `http://<host>:8080/?action=snapshot`. On success the
JPEG is base64-encoded (via `boost::beast::detail::base64`) into
`camera_jpeg_base64` and `camera_available` is set. Failure is silent — no
camera is the normal case.

**Rendering.** `to_prompt()` produces
`{"slicer": …, "live_printer": … (only when available), "camera_available": bool}`
as indented JSON. `to_image()` / `has_image()` expose the frame for multimodal
requests.

**Where it lands in the request.** `ai_generate_shape_to_plate()` puts
`to_prompt()` into the **system** message, after a directive that the model must
call `create_model` and must not exceed the build volume. The user's prompt goes
in the **user** message, with the camera frame attached to that message when one
exists. So the model sees the machine as authoritative constraints and the user
text as the request — which is what keeps generated geometry sized for *this*
printer rather than generically.

The build volume is *also* enforced independently of the prompt: the same call
computes bed extents from `printable_area` / `printable_height` via
`compute_bed_dims()` and validates the built model with
`ai_model_fits_bed()`. The prompt asks; the code checks.

---

## 5. Generation pipeline

`ai_generate_shape_to_plate(prompt, on_done)` is the one code path behind every
generation surface. It is safe to call from the main thread and always invokes
`on_done(ok, message)` on the main thread.

```
main thread                          worker Job thread                main thread
───────────                          ─────────────────                ───────────
create() → nullptr? → error
AIPrinterContext::gather()
build system+user messages
build forced-tool-call params
compute_bed_dims()
        │
        └── replace_job ──►  ┌─ attempt 0..2 ────────────────┐
                             │ provider->chat(msgs, params)  │
                             │ build model from tool call    │
                             │ validate: built? fits bed?    │
                             │ ok → done                     │
                             │ else → append repair message  │
                             └───────────────────────────────┘
                                            └──────────────►  take_snapshot()
                                                              model.add_object()
                                                              reload_scene()
                                                              on_done(ok, msg)
```

**Everything GUI-touching happens before the job starts.** Context gathering,
preset reads, and bed dimensions are captured on the main thread and moved into
the lambda by value (`shared_ptr` for the mutable message list and params). The
worker thread sees only plain data plus the provider.

**Forced tool call.** The request declares one function, `create_model`, whose
description embeds `ai_shape_spec_instructions()`, and sets `tool_choice` to
that function with `temperature: 0.2`. Requiring a structured tool call rather
than parsing prose is what makes the result reliable; a model that replies with
text instead has failed the task, and the pipeline says so explicitly rather
than retrying forever.

**Repair loop.** Up to `MAX_REPAIRS = 2` corrections (3 attempts total). After
each attempt the result is validated two ways — did the geometry build, and does
it fit the build volume? On failure the specific problem plus a truncated copy
of the model's previous output are appended as a new user turn and the model is
asked to correct itself. Network failures are *not* retried. If the last attempt
still overflows the bed but produced usable geometry, it is accepted with a
warning rather than discarded, so the user can rescale.

**Plate injection.** On success the main-thread continuation takes an undo
snapshot ("Add AI generated object"), copies each `ModelObject` into the plater
model, calls `ensure_on_bed()`, refreshes the object list, and reloads the 3D
scene.

**Cancellation.** The job checks `ctl.was_canceled()` after each round-trip and
reports progress at 15/45/70/100%. `AISlicerDialog` holds itself in a
`wxWeakRef` across the callback so closing the dialog mid-generation is safe.

---

## 6. `AIShapeGen` — the shape spec

The wire format between model and slicer is a small CSG tree in JSON:

```json
{"type":"difference","children":[
  {"type":"box","size":[30,30,30]},
  {"type":"cylinder","radius":5,"height":30}]}
```

- **Primitives**: `box`/`cube` (`size:[x,y,z]`), `cylinder` and `cone`
  (`radius` or `diameter`, `height`), `sphere` (`radius` or `diameter`).
- **Operations**: `union`, `difference`, `intersection`, each with a non-empty
  `children` array; `difference` subtracts every later child from the first.
- **Per-node transform**: `scale`, then `rotate` (degrees), then `translate`,
  applied via `Geometry::assemble_transform(..., fix_left_handed=true)`.
- Every primitive is generated **centered on the origin**, so a centered hole
  needs no `translate` — the single most common failure mode in LLM-authored
  CSG, removed by construction.
- Nesting is capped at `MAX_DEPTH = 64`; circle facets use a ~2° facet angle.

**Robustness against real model output.** `build_mesh()` accepts `operation`,
`op`, and `shape` as aliases for `type`, and `diameter` as an alias for
`radius`. `ai_build_model_from_response()` strips `<think>` / `<thinking>` /
`<reasoning>` blocks (reasoning models put draft JSON there), strips code
fences, then extracts the outermost `{ … }` from anywhere in the reply rather
than requiring JSON to start at byte 0. If that fails and the text looks like an
ASCII STL (`solid` + `facet`), it is loaded as raw STL through a temp file.
`ai_build_model_from_tool_call()` accepts either `{"shape": <node>}` or a bare
node at the top level.

**Booleans.** `csg_combine()` tries CGAL first and falls back to `mcut` if CGAL
throws. If mcut returns multiple disjoint volumes, the first is kept and the
rest are logged.

**Manifold repair.** CSG on coincident or flush faces routinely leaves
non-manifold edges that make a mesh unprintable. After building, every spec goes
through `its_merge_vertices()` + `its_remove_degenerate_faces()` and then a CGAL
`MeshBoolean::self_union()`, which resolves self-intersections and is a no-op on
an already-clean mesh. Repair is best-effort: if self-union throws, the cheaply
cleaned mesh is kept.

**Bed fit.** `ai_model_fits_bed()` merges the bounding boxes of all volumes and
compares the XY/Z extent against the build volume, producing a formatted
warning naming both sizes.

---

## 7. Configuration

All settings live in the `ai_slicer` section of the OrcaSlicer `AppConfig`. The
section is free-form — no schema registration is needed and no profile,
project-file, or preset format is touched.

| Key | Values | Meaning |
|---|---|---|
| `ai_slicer/provider` | `""` (disabled), `openai`, `anthropic`, `compatible` | Which backend to instantiate. `openai_compatible` is accepted as a synonym of `compatible` when reading. |
| `ai_slicer/api_key` | string | Provider credential. Sent as `Authorization: Bearer` (OpenAI-shaped) or `x-api-key` (Anthropic). |
| `ai_slicer/gateway_url` | URL | Base URL. **Required** for `compatible`; overrides the default endpoint for the other providers when set. |
| `ai_slicer/model` | model id | Defaults if empty: `gpt-4o-mini` (OpenAI-shaped), `claude-3-5-sonnet-latest` (Anthropic). |

`AIProvider::config_from_app_config()` is the only reader; the Preferences page
is the only writer. The section is rendered with the page's standard row
builders — the section-aware overloads of `create_item_combobox()`,
`create_item_input()` (with a `password` flag for the key), `create_item_button()`,
and `create_item_warning()` for the always-visible cleartext-key notice — so it
inherits the page's persistence behaviour: each field writes and
`app_config->save()`s on `wxEVT_TEXT_ENTER` and `wxEVT_KILL_FOCUS`.

For backward compatibility, opening Preferences rewrites a stored
`provider = openai_compatible` to `compatible`, so configs written by earlier
builds select the right combo entry. Both spellings remain accepted by
`AIProvider::create()`.

**Absent keys mean off.** A config with no `ai_slicer` section yields an empty
`provider`, `create()` returns `nullptr`, and every entry point reports "no
provider configured" without touching the network.

---

## 8. Adding a provider

The abstraction is designed so a new backend is one class plus one factory
branch. Nothing in the context, the shape core, or any UI is provider-aware.

1. **Implement the interface** in the anonymous namespace of
   `src/slic3r/Utils/AIProvider.cpp`, next to `OpenAIProvider` and
   `AnthropicProvider`:

   ```cpp
   class MyProvider : public AIProvider {
   public:
       explicit MyProvider(AIConfig cfg)
           : m_cfg(std::move(cfg))
           , m_base(strip_trailing_slash(m_cfg.base_url.empty()
                                         ? "https://api.example.com" : m_cfg.base_url)) {}

       const char *get_name() const override { return "Example"; }

       AIResponse chat(const std::vector<AIMessage> &messages,
                       const nlohmann::json &params) const override;

       bool test_connection(std::string &err) const override
       { std::vector<AIModelInfo> m; return get_models(m, err); }

       bool get_models(std::vector<AIModelInfo> &models, std::string &err) const override;
   private:
       AIConfig m_cfg; std::string m_base;
   };
   ```

2. **In `chat()`**, reuse the file-local helpers: `api_url()` for endpoint
   joining, `http_send()` for the synchronous round-trip, `status_ok()` and
   `http_error_message()` for the failure path. Then:
   - Translate `AIMessage` into the provider's message shape, including the
     multimodal form for `images`.
   - Translate the incoming `params` — in particular `tools` / `tool_choice`,
     which arrive in **OpenAI function-calling shape** (see the limitation in
     §10). If the provider uses a different schema, rewrite them here; this is
     the correct place for that translation.
   - Populate `AIResponse::content`, and `tool_call_name` /
     `tool_call_arguments` when the reply is a tool call. Set `ok = true` only
     after parsing succeeded.

3. **Add one branch** to `AIProvider::create()`:

   ```cpp
   if (provider == "example")
       return new MyProvider(config);
   ```

4. **Expose it in Preferences** (`src/slic3r/GUI/Preferences.cpp`): append the
   display name to the *AI provider* combo and add the matching case to
   `ai_provider_index()` / `ai_provider_key()`. These two functions are the
   whole index↔key mapping.

That is the complete set of edits. The dialogs, the sidebar, the context
assembly, and the shape core need no changes, because they only ever see
`AIProvider` and `AIResponse`.

---

## 9. UI surfaces

| Surface | Where | Notes |
|---|---|---|
| Sidebar generator | Top of the parameter panel, all platforms | Prompt field + **Generate**; Enter also submits. Status line turns green on success, red on failure. |
| **AI ▸ Generate 3D Shape…** | Menu bar | Opens `AISlicerDialog`. Currently built only in the macOS branch of `MainFrame::init_menubar()` (see §10). |
| `AISlicerDialog` *Generate shape* | Dialog tab | Multiline prompt, **Generate**, status line, and copy stating the scope: parametric mechanical parts, not organic shapes. |
| `AISlicerDialog` *Find models* | Dialog tab | Query field + one button per site (Printables, MakerWorld, Thingiverse, Thangs). Opens that site's real search page in the browser. |
| Preferences *General ▸ AI Slicer* | Settings | Provider combo, gateway URL, API key (masked) with an inline cleartext warning, model, then the *AI connection* row (**Test connection**) and the *AI 3D generation* row (**Qualify for 3D**), each with its own status label. |

**Why *Find models* opens a browser instead of downloading.** An LLM has no live
web access, and the model repositories gate downloads behind sign-in and
anti-bot checks, so asking a model for a direct download URL reliably yields
403/404/DNS errors. Handing the query to each site's own search page keeps the
user in their normal, signed-in download flow; the file is then loaded with
*File ▸ Import* like any other model. This is a deliberate reduction in scope
from an earlier "search & import" design that could not work.

**Qualify for 3D** is a distinct check from **Test connection**. Test connection
only proves the endpoint and key (`get_models()`). Qualify sends a canonical
prompt — "a 30mm cube with a 10mm hole through the center" — with a forced
`create_model` call, then actually builds the returned spec through
`ai_build_model_from_tool_call()` and reports the round-trip time. It answers
the question that matters: *can this particular model produce buildable
geometry?*

---

## 10. Known limitations

These are properties of the code as it stands, listed so the docs do not
overstate what ships.

1. **The AI menu is macOS-only.** In `MainFrame::init_menubar()` the menu is
   appended inside the `#else` half of `#ifndef __APPLE__`, so Windows and Linux
   builds — which use the `m_topbar` path — get no **AI** menu and therefore no
   access to `AISlicerDialog` or its *Find models* tab. The sidebar generator is
   unconditional, so shape generation itself works everywhere. The Preferences
   tooltip that points at "the AI menu" is misleading on those platforms.
   *Fix:* also append the menu (or a topbar entry) in the non-Apple branch.

2. **Anthropic cannot complete a generation.** `ai_generate_shape_to_plate()`
   and `ai_qualify_model()` build `tools` / `tool_choice` in OpenAI
   function-calling shape (`{"type":"function","function":{…}}`), and
   `AnthropicProvider::chat()` merges `params` into the request body verbatim.
   The Anthropic Messages API expects flat tool objects
   (`{"name","description","input_schema"}`) and a different `tool_choice`
   shape, so the request is rejected. `AnthropicProvider` *can* read a
   `tool_use` reply — only the request-side translation is missing.
   *Fix:* translate `tools`/`tool_choice` inside `AnthropicProvider::chat()`,
   which is where every other provider-specific shape difference is handled.

3. **`AISettingsDialog` is dead code.** `src/slic3r/GUI/AISettingsDialog.{hpp,cpp}`
   is compiled and included by `MainFrame.cpp` but never instantiated; the
   settings UI it duplicates now lives in Preferences, and its
   `run_qualification()` is a near-copy of `ai_qualify_model()`.
   *Fix:* delete the pair, drop it from `src/slic3r/CMakeLists.txt`, and remove
   the include.

4. **Context gathering blocks the UI thread.** `AIPrinterContext::gather()`
   performs the live-telemetry and camera requests synchronously, and it is
   called on the main thread before the job is queued. Each has a 3-second
   *connect* timeout but no overall transfer cap, so an unresponsive host can
   stall the UI. *Fix:* move `gather()` onto the worker thread, passing the
   already-read preset data in.

5. **OctoPrint hosts are queried with a Moonraker URL.**
   `/printer/objects/query?…` is Moonraker's endpoint; OctoPrint exposes
   `/api/printer`. `htOctoPrint` takes the same branch, so live telemetry is
   simply unavailable for OctoPrint (it degrades cleanly, but the branch
   promises more than it delivers).

6. **The camera probe is hard-coded.** Host is derived from `print_host` and the
   URL is fixed at `http://<host>:8080/?action=snapshot`; any configured camera
   URL, alternate port, or HTTPS camera is ignored.

7. **No automated test coverage.** Nothing under `tests/` exercises the AI code.
   `AIShapeGen` and `AIPrinterContext::slicer_context_from_config()` are pure
   and are the obvious first targets — see the test plan in
   [UPSTREAM-PR.md](UPSTREAM-PR.md).

8. **`AISlicerDialog.cpp` strings reach `_L()` but not the catalogue.** The file
   is now listed in `localization/i18n/list.txt`; before that its strings were
   wrapped for translation but never extracted into the POT.

---

## 11. Security and privacy properties

- The API key is stored **in cleartext** in the OrcaSlicer config file, the same
  way existing print-host credentials are. The Preferences field is masked on
  screen (`wxTE_PASSWORD`) — that is display masking, not storage encryption.
  Because a tooltip is easy to never open, the consequence is spelled out in a
  warning line rendered directly beneath the field. Use a scoped, revocable key.
- Every generation sends the machine context — printer, material, and process
  settings, plus live telemetry and a camera frame when those are available — to
  the configured provider. For a self-hosted gateway that traffic never leaves
  the local network; for a hosted provider it does.
- Nothing is transmitted until a provider is configured *and* the user invokes
  an AI action. There is no background activity, no telemetry, and no
  Orca-operated endpoint.
- *Find models* only opens a URL in the user's browser. No file is fetched by
  the slicer and no model-supplied URL is ever followed.
- Generated STL fallbacks are written to a unique temp file, read through the
  normal `TriangleMesh::ReadSTLFile()` path, and removed immediately.

---

## 12. Delivery

The feature was built as a sequence of small, individually compiling changes,
each landing one layer of the map in §2: provider abstraction → machine context
and multimodal support → settings UI → shape core → generation dialog → menu →
sidebar entry point → manifold repair → documentation. That ordering keeps every
intermediate state buildable and makes the stack reviewable one layer at a time.
See [UPSTREAM-PR.md](UPSTREAM-PR.md) for the proposed upstream submission.
