# Upstream PR — AI Slicer

Proposed pull request from `jordanhubbard/OrcaSlicer` to
**`OrcaSlicer/OrcaSlicer` : `main`**.

The body below follows this repository's `.github/pull_request_template.md`
(Description → Screenshots → Tests) with the additional sections a reviewer will
want for a network-touching, opt-in feature. Copy everything between the rules
into the PR body.

> **Before opening the PR**, resolve the blockers in
> [§ Must-fix before submission](#must-fix-before-submission) at the end of this
> file. They are known defects in the current tree, and a reviewer will find them.

---

# Description

## What this adds

An optional, provider-agnostic AI assistant. With a gateway registered, the user
can **describe a mechanical part in plain language and have it generated onto the
plate**, sized against the machine that will actually print it. A second tab
helps find ready-made models on the community sites when a part is too organic
to generate.

The feature is **off by default and inert until configured**. It adds **no new
third-party dependencies** — HTTP reuses `Slic3r::Http`, JSON reuses the
vendored `nlohmann::json`, geometry reuses `libslic3r`. It does not touch the
slicing pipeline, profiles, project files, or any existing default.

Architecture and internals: [`doc/ai-slicer/PLAN.md`](PLAN.md).
User guide: [`doc/ai-slicer/README.md`](README.md).

## The three pieces

**1. Text → printable geometry.** The model is required to answer with a forced
`create_model` tool call carrying a compact CSG tree — primitives (box,
cylinder, cone, sphere) combined with union / difference / intersection plus
per-node transforms — never prose. `AIShapeGen` validates that spec and bakes it
into a `Model`. Requiring structure rather than parsing prose is what makes the
result reliable, and it gives a clean signal for models that are not up to the
task.

Framing is deliberately honest to what the code does: this is a **parametric**
generator for mechanical parts. It cannot produce organic or sculptural shapes,
and both the UI copy and the system prompt say so rather than letting the user
discover it through bad output.

**2. Generation grounded in the real machine.** Every request carries an
`AIPrinterContext`: bed polygon and height, nozzle, G-code flavour, machine
speed/acceleration limits, filament and temperatures, layer heights and infill
(always); live Moonraker telemetry when a Physical Printer is selected; and a
camera frame for vision models when one is reachable. Anything unavailable is
omitted — offline generation still gets the full slicer-side context.

The build volume is enforced **twice and independently**: the model is
instructed not to exceed it, and `ai_model_fits_bed()` measures the result
afterwards. A model that ignores the instruction is still caught.

**3. Self-correction and manifold repair.** If the geometry fails to build or
overflows the bed, the specific problem is fed back and the model is asked to
correct itself — up to two repairs, three attempts total. Network failures are
not retried. A final result that is oversized but usable is accepted with a
warning rather than discarded, so the user can rescale instead of starting over.
Because CSG on coincident faces routinely leaves non-manifold edges, every built
spec goes through vertex merge + degenerate-face removal + a CGAL `self_union()`
(best-effort, no-op on clean meshes) so the object is actually printable.

## User-visible surfaces

| Surface | Location |
|---|---|
| **AI part generator** — prompt + Generate | Top of the sidebar parameter panel, all platforms |
| **AI ▸ Generate 3D Shape…** | Menu bar → two-tab dialog (*Generate shape*, *Find models*) |
| **Preferences ▸ General ▸ AI Slicer** | Provider, gateway URL, API key (masked, with an inline cleartext warning), model, **Test connection**, **Qualify for 3D** |

**Test connection** proves the endpoint and key. **Qualify for 3D** answers the
question that actually matters — it sends a canonical prompt with a forced tool
call and *builds* the result, reporting round-trip time. Many models pass the
first and fail the second; surfacing that up front avoids a class of "the AI is
broken" reports.

**Find models** deliberately hands the query to each site's own search page in
the user's browser (Printables, MakerWorld, Thingiverse, Thangs) rather than
downloading. A plain LLM has no live web access and the repositories gate
downloads behind sign-in and anti-bot checks, so model-supplied download URLs do
not work. The browser handoff keeps the user in their signed-in flow, and as a
side effect the slicer never fetches a file from a URL a model invented.

## Architecture

`AIProvider` mirrors the existing `PrintHost::get_print_host()` pattern: an
abstract interface plus a `create()` factory, with the concrete `OpenAIProvider`
and `AnthropicProvider` confined to the anonymous namespace of the `.cpp`.
Callers never name a concrete provider. `AIShapeGen` and
`AIPrinterContext::slicer_context_from_config()` are pure — no GUI, no network —
so the geometry and context logic are unit-testable and wx-free.

`ai_generate_shape_to_plate()` (`GUI/AIGenerate.hpp`) is the **single** pipeline
behind every surface, so the sidebar and the dialog cannot drift apart. It runs
the LLM round-trip and geometry build on the existing `Job`/`Worker`
infrastructure, capturing all preset and GUI reads on the main thread before the
job starts.

Adding a provider is one class plus one factory branch plus two lines in the
Preferences combo mapping — nothing else in the codebase is provider-aware. The
procedure is written out in [`PLAN.md`](PLAN.md) § 8.

## Files

New (`~1,650` lines including comments):

```
src/slic3r/Utils/AIProvider.{hpp,cpp}          145 / 335
src/slic3r/Utils/AIPrinterContext.{hpp,cpp}     57 / 192
src/slic3r/Utils/AIShapeGen.{hpp,cpp}           64 / 379
src/slic3r/GUI/AIGenerate.hpp                   20
src/slic3r/GUI/AISlicerDialog.{hpp,cpp}         54 / 402
doc/ai-slicer/{README,PLAN,UPSTREAM-PR}.md
```

`src/slic3r/GUI/AISettingsDialog.{hpp,cpp}` (49 + 309) also exists in the branch
but is unreachable and slated for deletion before submission — see must-fix #3.

Modified: `src/slic3r/CMakeLists.txt` (source registration),
`src/slic3r/GUI/MainFrame.cpp` (AI menu), `src/slic3r/GUI/Plater.cpp` (sidebar
generator), `src/slic3r/GUI/Preferences.{cpp,hpp}` (*General ▸ AI Slicer*
section, plus three additive row builders — a section-aware `create_item_combobox`
and `create_item_input`, and `create_item_warning`),
`localization/i18n/list.txt` (translation extraction).

The Preferences section is deliberately built from that page's standard row
builders rather than hand-rolled sizers, so it inherits the page's layout,
dark-mode, tooltip, and persistence behaviour. The three new builders are
additive overloads/helpers; no existing call site changes.

## Compatibility and blast radius

- **No behaviour change when disabled.** With `ai_slicer/provider` empty —
  the default, and the state of every existing config —
  `AIProvider::create()` returns `nullptr` and every entry point reports "not
  configured" without touching the network. The only visible addition is an
  inert prompt box in the sidebar.
- **No format changes.** Settings live in a free-form `ai_slicer` section of
  `AppConfig`. No profile, preset, `.3mf`, or G-code format is touched, so no
  version migration is needed and project backward compatibility is unaffected.
- **No slicing changes.** Nothing in `libslic3r`'s slicing path is modified;
  `AIShapeGen` only *consumes* existing mesh and boolean utilities.
- **Cross-platform.** wxWidgets and `Slic3r::Http` only; no platform-specific
  API. (One platform gap in the menu wiring is listed as a must-fix below.)
- **New dependencies: none.**

## Design decisions worth flagging to reviewers

1. **Forced tool call instead of prose parsing.** Structured output is the only
   reliable path, and "did not call the tool" is a crisp unsuitable-model signal
   rather than a mystery failure.
2. **Parametric-only, stated plainly.** Scope is limited to what CSG over
   primitives can express. The alternative — implying general 3D generation —
   would generate support requests, not parts.
3. **Browser handoff for model search.** Chosen after the download-directly
   approach proved unworkable against real sites; see above.
4. **Config in `AppConfig`, not `PrintConfig`.** These are application
   preferences, not print settings, so they must not enter profiles or project
   files.
5. **Two separate validation buttons.** Connectivity and capability are
   different failures and deserve different messages.

# Screenshots/Recordings/Graphs

<!-- Attach before opening the PR. -->

- [ ] **Preferences ▸ General ▸ AI Slicer** — the full section (provider combo,
      gateway URL, masked API key with the orange cleartext warning beneath it,
      model, both buttons), in light *and* dark mode.
- [ ] **Qualify for 3D — pass**, showing the green
      `Qualified for 3D generation (N.Ns). This model works.` result.
- [ ] **Qualify for 3D — fail**, showing the
      `Not suitable — the model replied with text…` message (the common case
      worth documenting).
- [ ] **Sidebar generator** with a prompt typed and the `Generating…` status.
- [ ] **Before/after on the plate** — prompt visible, resulting object on the
      bed. Suggested: `a 40 mm hex knob with a 6 mm shaft hole`.
- [ ] **AI ▸ Generate 3D Shape… dialog**, *Generate shape* tab, including the
      scope note.
- [ ] ***Find models*** tab with the four site buttons.
- [ ] **Oversized-result warning** — object added with the
      `exceeds the build volume` message.
- [ ] **Repair loop** — short recording of a first attempt failing and the
      corrected second attempt landing on the plate.
- [ ] **Disabled state** — the sidebar box with `provider = Disabled`,
      demonstrating the no-op path.

## Tests

### Manual verification

| # | Scenario | Expected |
|---|---|---|
| 1 | Fresh config, provider `Disabled` | No network traffic; sidebar Generate reports "No AI provider is configured…"; app otherwise unchanged |
| 2 | OpenAI provider + valid key → **Test connection** | `Connection OK.` |
| 3 | Wrong key | `Authentication failed (401)…` |
| 4 | Bad gateway URL | `Endpoint not found (404) — check the gateway URL.` |
| 5 | Compatible provider, gateway URL empty | Feature stays disabled; reason logged |
| 5b | Config written by an older build with `provider = openai_compatible` | Migrated to `compatible` on opening Preferences; combo shows *OpenAI-compatible* |
| 6 | Gateway URL ending in `/v1` vs not | Both resolve correctly (no doubled `/v1`) |
| 7 | **Qualify for 3D**, capable model | Green pass with timing |
| 8 | **Qualify for 3D**, prose-only model | `Not suitable — the model replied with text…` |
| 9 | Sidebar: `a 30 mm cube with a 10 mm hole through the centre` | Object on plate, hole centred, single undo step |
| 10 | Dialog *Generate shape*, same prompt | Identical result (shared pipeline) |
| 11 | Prompt for an oversized part on a small bed | Repair attempted; if still oversized, added with the build-volume warning |
| 12 | Organic prompt (`a cat figurine`) | Clear unsupported-scope message, no bogus geometry |
| 13 | Generation with no Physical Printer selected | Succeeds; context contains slicer settings only |
| 14 | Generation with a Moonraker host reachable | `live_printer` present in the context |
| 15 | Moonraker host configured but offline | Degrades cleanly after the connect timeout; generation still succeeds |
| 16 | Camera reachable, vision model | Frame attached; `camera_available: true` |
| 17 | Close the dialog mid-generation | No crash (weak-ref guard); result discarded |
| 18 | Undo after a generation | Object removed in one step |
| 19 | Reasoning model emitting `<think>` blocks | Blocks stripped; spec parsed |
| 20 | Model returning raw ASCII STL | STL fallback path loads it; temp file removed |

### Automated tests

**None yet — this is the main gap in the change, and it is acknowledged rather
than hidden.** Nothing under `tests/` exercises the AI code today.

The two pure, network-free, GUI-free units are the right first targets and need
no new infrastructure — they drop into the existing Catch2 `tests/libslic3r`
suite:

- **`AIShapeGen`** — primitives and their aliases (`cube`/`box`,
  `diameter`/`radius`); `union` / `difference` / `intersection`; per-node
  `translate` / `rotate` / `scale`; centred-at-origin invariant; rejection of
  non-positive sizes, zero scale, missing `type`, empty `children`, and
  over-deep nesting; `<think>`-block and code-fence stripping; JSON embedded in
  prose; ASCII-STL fallback; `{"shape": …}` vs bare-node tool arguments;
  `ai_model_fits_bed()` boundary cases; manifold output after a flush-face cut.
- **`AIPrinterContext::slicer_context_from_config()`** — expected keys present
  from a known config; missing options omitted rather than crashing; stable
  serialization of vector and enum options.

Provider HTTP paths need a stub server or an injected transport and are better
left to a follow-up; the response *parsers* could be split out and tested
directly.

## Security & privacy

**Key storage is the one thing a reviewer must consciously accept.** The API key
is written **in cleartext** to the OrcaSlicer config file, exactly like existing
print-host credentials. The Preferences field uses `wxTE_PASSWORD`, which is
display masking only — not encryption at rest. Rather than bury that in a
tooltip, the consequence is stated in an always-visible warning line directly
beneath the field, and again in the user guide, which recommends a scoped,
revocable key and a local gateway for users who cannot accept it. An OS
secret-store backend is the obvious follow-up and is deliberately out of scope
for this change.

**Data leaving the machine.** Nothing is transmitted until a provider is
configured *and* the user invokes an AI action. There is no background traffic,
no telemetry, and no Orca-operated endpoint — the user brings their own. When a
generation runs, the request carries the prompt plus the machine context;
with a Physical Printer configured that includes live telemetry, and with a
camera reachable it includes a **snapshot of the printer and whatever is in
front of it**. Both are stated in the user guide. A self-hosted gateway keeps all
of it on the local network.

**Attack surface.**

- No model-supplied URL is ever fetched. *Find models* only opens a URL the
  slicer constructed from a user query in the user's browser, with the query
  percent-encoded.
- Model output is parsed as JSON into a strictly validated spec — unknown node
  types, bad sizes, zero scales, and over-deep nesting are rejected. Geometry is
  built only from that validated tree.
- The raw-STL fallback writes to a unique temp file, reads it through the normal
  `TriangleMesh::ReadSTLFile()` importer, and deletes it immediately.
- All parsing is exception-guarded; failures become user-facing messages, never
  crashes.
- Boolean operations that fail or crash CGAL fall back to mcut, and a failed
  fallback is reported rather than producing a corrupt mesh.

**Opt-in guarantee.** `provider` empty ⇒ `AIProvider::create()` returns
`nullptr` ⇒ every call site returns early with "not configured". That is the
single choke point, and it is checked on the main thread before any context is
gathered.

## Review checklist

Regressions and compatibility

- [ ] With `ai_slicer/provider` unset, no AI code path performs I/O and no
      existing behaviour, default, profile, or project-file interaction changes.
- [ ] No `.3mf`, preset, profile, or G-code format change; no migration needed.
- [ ] No modification to the slicing pipeline; `libslic3r` changes are additive
      only.
- [ ] Existing configs load unchanged; the unknown `ai_slicer` section is
      harmless on downgrade.
- [ ] The new Preferences row builders are additive overloads/helpers; no
      existing Preferences row changes appearance or behaviour.

Build and platform

- [ ] Builds clean on Windows, macOS, and Linux with no new warnings.
- [ ] All new sources registered in `src/slic3r/CMakeLists.txt`.
- [ ] No new third-party dependency; `deps/` untouched.
- [ ] **The AI menu is reachable on all three platforms** (see must-fix #1).

Correctness

- [ ] Provider selection, including `compatible` without a gateway URL, behaves
      as documented.
- [ ] `/v1` joining is correct for bases with and without the suffix.
- [ ] **Anthropic completes a generation end-to-end** (see must-fix #2).
- [ ] The repair loop terminates (`MAX_REPAIRS = 2`) and does not retry network
      failures.
- [ ] Bed-fit checking uses the active machine's `printable_area` /
      `printable_height`.
- [ ] Generated meshes are manifold after boolean cuts with coincident faces.

Threading and lifetime

- [ ] All preset/GUI reads happen on the main thread before the job is queued;
      the worker lambda captures only owned data.
- [ ] Cancellation is honoured and closing the dialog mid-generation is safe.
- [ ] Plate mutation, undo snapshot, and scene reload happen on the main thread.
- [ ] **Context gathering does not block the UI thread** (see must-fix #4).

Style, i18n, and docs

- [ ] Matches surrounding style; C++17; `#pragma once` or include guards
      consistent with neighbours.
- [ ] Every user-facing string is wrapped in `_L` / `_u8L`, and every file
      containing them is listed in `localization/i18n/list.txt`. (Today
      `AISlicerDialog.cpp` is listed; `AISettingsDialog.cpp` is not, and its 28
      `_L()` strings are therefore not extracted — resolved by must-fix #3
      deleting that file, not by listing it.)
- [ ] **No dead code** (see must-fix #3).
- [ ] `doc/ai-slicer/` accurately describes the merged behaviour, including the
      cleartext-key warning.

Security

- [ ] Cleartext key storage is an accepted, documented trade-off consistent with
      existing print-host credential handling.
- [ ] No model-supplied URL is fetched by the slicer.
- [ ] Model output is validated before it becomes geometry; all parsers are
      exception-guarded.

## Known follow-ups (out of scope here)

- OS secret-store backend for the API key.
- Automated tests for the provider HTTP layer behind an injectable transport.
- Streaming responses and a cancel-during-request affordance.
- OctoPrint-native telemetry (`/api/printer`) instead of the Moonraker query.
- Configurable camera URL rather than the hard-coded mjpg-streamer default.
- Richer shape vocabulary — fillets, chamfers, threads, text.

---

## Must-fix before submission

These are real defects in the current tree, found while auditing the code
against the documentation. They are listed here so they are not discovered by an
upstream reviewer first. Each is also recorded in [`PLAN.md`](PLAN.md) § 10.

1. **The AI menu never appears on Windows or Linux.** In
   `MainFrame::init_menubar()` the menu is appended inside the `#else` half of
   `#ifndef __APPLE__`, i.e. the macOS-only branch. Non-Apple builds use the
   `m_topbar` path and get no **AI** menu, so `AISlicerDialog` and the entire
   *Find models* tab are unreachable there. The sidebar generator still works.
   The Preferences tooltip referring to "the AI menu" is also wrong on those
   platforms. → Append the menu (or a topbar entry) in the non-Apple branch too.

2. **Anthropic cannot complete a generation.** Both
   `ai_generate_shape_to_plate()` and `ai_qualify_model()` build `tools` /
   `tool_choice` in OpenAI function-calling shape
   (`{"type":"function","function":{…}}`), and `AnthropicProvider::chat()`
   merges `params` into the request body verbatim. The Messages API expects flat
   tool objects (`{"name","description","input_schema"}`) and a different
   `tool_choice` shape, so the request is rejected — the provider is advertised
   in the UI but non-functional for the feature's primary path. The response
   side already handles `tool_use` correctly; only request translation is
   missing. → Translate `tools`/`tool_choice` inside `AnthropicProvider::chat()`.

3. **`AISettingsDialog` is dead code.**
   `src/slic3r/GUI/AISettingsDialog.{hpp,cpp}` (49 + 309 lines) is compiled and
   included by `MainFrame.cpp` but never instantiated; the settings UI moved to
   Preferences, and its `run_qualification()` duplicates `ai_qualify_model()`.
   Upstream review explicitly flags duplication and dead code. → Delete both
   files, drop them from `src/slic3r/CMakeLists.txt`, and remove the include.

4. **Context gathering blocks the UI thread.** `AIPrinterContext::gather()` runs
   its live-telemetry and camera requests synchronously and is called on the
   main thread before the job is queued. Each has a 3-second *connect* timeout
   but no overall transfer cap, so a slow or half-open host stalls the UI —
   avoidable, given the pipeline already has a worker thread. → Move `gather()`
   into the job, passing the main-thread preset reads in as plain data.

5. **OctoPrint hosts take the Moonraker branch.**
   `/printer/objects/query?…` is Moonraker-specific; OctoPrint exposes
   `/api/printer`. `htOctoPrint` is routed to the Moonraker URL, so its live
   telemetry silently never populates. It degrades cleanly, so this is a
   correctness/clarity fix rather than a crash. → Either implement the OctoPrint
   query or restrict the branch to `htMoonraker`.
