# AI Slicer — user guide

An optional AI assistant built into OrcaSlicer. Register an LLM gateway — OpenAI,
Anthropic, or any OpenAI-compatible endpoint including a local one — and then
**describe a mechanical part in plain language and get it on the plate**, sized
for your actual printer. A second tab helps you **find a ready-made model** on
the community sites when a part is too organic to generate.

The feature is entirely **opt-in**. Until you configure a provider, nothing
contacts a network and no AI action does anything but tell you it is not
configured.

- Architecture and internals: [PLAN.md](PLAN.md)
- Upstreaming proposal: [UPSTREAM-PR.md](UPSTREAM-PR.md)

---

## 1. Register a gateway — *Preferences ▸ General ▸ AI Slicer*

All AI settings live in Preferences, under the **AI Slicer** heading in the
General page, in this order:

| Field | What to put in it |
|---|---|
| **AI provider** | `Disabled`, `OpenAI`, `Anthropic`, or `OpenAI-compatible (custom gateway)`. `Disabled` turns the feature off completely. |
| **Gateway URL** | Base URL of an OpenAI-compatible endpoint, e.g. `https://host/v1/`. **Required** for the compatible provider; leave empty for OpenAI and Anthropic unless you are routing them through a proxy. |
| **API key** | Your provider credential. The field is masked as you type, and a warning below it restates that the key is stored as clear text. See [Security](#4-security-and-privacy) before you paste a key. |
| **Model** | The model id, e.g. `gpt-4o-mini` or `claude-3-5-sonnet-latest`. Leave empty to use the provider default. |

Each field saves as soon as you press Enter or click away — there is no separate
Save button. If your config predates this layout and stored the provider as
`openai_compatible`, it is migrated to `compatible` the first time you open
Preferences, so the combo box shows the right entry.

### Using a custom or local gateway

Choose **OpenAI-compatible (custom gateway)** and set **Gateway URL** to your
endpoint. Anything that speaks the OpenAI chat-completions API works: Ollama,
LiteLLM, vLLM, an Azure/OpenAI proxy, a corporate gateway. A URL that already
ends in `/v1` is handled correctly — it will not be doubled. If you select this
provider without a gateway URL, the feature stays off and logs why.

Local gateways are the privacy-preserving option: your prompts and machine
context never leave your network.

### Check it works — two buttons, two different questions

**Test connection** (on the *AI connection* row) asks *are the endpoint and key
valid?* It lists the models your key can reach and reports `Connection OK.` or
the specific failure (authentication, wrong URL, rate limit).

**Qualify for 3D** (on the *AI 3D generation* row) asks the question that
actually matters: *can this model generate geometry?* It sends a canonical request — a 30 mm cube with a 10 mm
hole through the centre — requiring a structured tool call, then tries to build
the result. You get one of:

- `Qualified for 3D generation (2.4s). This model works.` — you are set.
- `Not suitable — the model replied with text instead of a create_model tool
  call.` — the model talks instead of producing structure. Pick another.
- `Returned a tool call, but its shape wasn't buildable: …` — close, but the
  geometry was invalid.

**Run *Qualify for 3D* before you rely on the feature.** Many models pass Test
connection and still fail here. Use a mid-size or larger instruct or code model;
embedding, reranker, and very small models will not qualify.

---

## 2. Generate a shape from a description

There are two ways in, and both run the identical pipeline.

**Sidebar (all platforms).** At the top of the parameter panel on the right
there is an **AI part generator (mechanical shapes)** box: type a prompt, press
Enter or click **Generate**.

**Dialog.** *AI ▸ Generate 3D Shape…* opens a larger window with a multiline
prompt on the **Generate shape** tab.

> **Note:** the **AI** menu is currently only present in macOS builds. On
> Windows and Linux, use the sidebar generator — it is the same pipeline and the
> same result. See *Known limitations* in [PLAN.md](PLAN.md).

Generation runs in the background; the rest of the app stays usable, and a
status line reports progress and the outcome. On success the object is added to
the plate, placed on the bed, and selected — under a single undo step, so one
Ctrl/Cmd-Z removes it.

### What it is good at

The generator builds **parametric parts**: boxes, cylinders, cones, and spheres
combined with boolean union, difference, and intersection, plus rotation,
scaling, and translation. That covers a large share of everyday functional
printing:

- `a 40 mm hex knob with a 6 mm shaft hole`
- `a bracket with four M3 holes, 60 x 40 x 5 mm`
- `a 100 mm cable clip, 8 mm inner diameter, open on one side`
- `a stackable box, 80 x 60 x 40 mm, 2 mm walls`

### What it is not good at

Anything organic or sculptural — animals, figurines, faces, freeform surfaces.
Those cannot be expressed as primitives and booleans, and the generator will
tell you so rather than produce something wrong. Use the **Find models** tab
instead.

### How your printer shapes the result

Every request carries a snapshot of *your* machine, which is why the output is
sized to print here rather than generically:

- **Always:** bed shape and size, maximum height, nozzle diameter and type,
  G-code flavour, machine speed and acceleration limits, filament type and
  diameter, nozzle and bed temperatures, layer heights, and infill density.
- **When a Physical Printer is configured** (Moonraker/OctoPrint host): live
  telemetry — current temperatures, toolhead state, bed mesh, job state.
- **When a camera is reachable** at the mjpg-streamer default
  (`http://<printer-host>:8080/?action=snapshot`) and the model supports vision:
  a single snapshot frame.

Anything unavailable is simply omitted — generating fully offline still works
and still gets the complete slicer-side context.

The build volume is enforced twice: the model is told not to exceed it, and the
result is measured against it after the fact. Those are independent, so a model
that ignores the instruction is still caught.

### If the first attempt is wrong

The generator self-corrects. If the geometry fails to build or overflows the
bed, the specific problem is fed back and the model is asked to fix it — up to
two corrections (three attempts total). Only genuine failures surface to you:

- **Oversized but usable:** the object is added anyway with a warning naming its
  size and your build volume, so you can scale it down rather than start over.
- **`This model returned no shape…`:** the model answered in prose. Run
  *Qualify for 3D* and switch models.
- **`Couldn't produce a valid shape after 3 tries…`:** the last problem is
  quoted. Rephrase with explicit dimensions in millimetres — concrete numbers
  work far better than adjectives.
- **`AI request failed: …`:** a network or credentials problem, not a modelling
  one. Network failures are not retried.

### Tips

- Give dimensions in millimetres and name the features: *"a 30 mm cube with a
  10 mm hole through the centre"* beats *"a cube with a hole"*.
- Describe the part as primitives and cuts — that is what the generator builds.
- Nothing is committed until you like it: undo removes the object in one step.

---

## 3. Find a model on the web — *Find models* tab

Open *AI ▸ Generate 3D Shape…* and switch to **Find models**. Type what you are
looking for, then click **Printables**, **MakerWorld**, **Thingiverse**, or
**Thangs** — Enter opens Printables. Your query opens in that site's own search
page in your browser. Download the `.stl` or `.3mf` there, then load it with
*File ▸ Import*.

**Why it hands off to the browser rather than downloading for you.** A language
model has no live web access, and the model repositories gate downloads behind
sign-in and anti-bot checks. Asking a model for a direct download link produces
broken URLs. Opening the site's real search keeps you in your normal, signed-in
flow, where downloads actually work. As a side effect, the slicer never fetches
a file from a URL a model invented.

---

## 4. Security and privacy

**Your API key is stored in cleartext.** It is written to the OrcaSlicer config
file exactly like existing print-host credentials. The Preferences field is
masked on screen, but that is display masking only — it is not encrypted at
rest, and anyone who can read your config file, your user profile, or a backup
of it can read the key. Preferences states this in a warning line directly under
the field rather than hiding it in a tooltip.

What to do about it:

- Use a **scoped, revocable** key, not an account-wide one.
- Prefer a key with a spending cap where the provider offers one.
- Treat the config file as a secret: do not commit it, sync it to a shared
  location, or include it in a bug report.
- If you cannot accept cleartext storage, run a **local gateway** and use a
  throwaway key, or leave the provider set to `Disabled`.

**What gets sent, and when.** Nothing is transmitted until you configure a
provider *and* invoke an AI action — there is no background traffic and no
Orca-operated service. When you do generate, the request contains your prompt
plus the machine context described above: printer, material, and process
settings, and — if a Physical Printer is configured — live telemetry and a
camera frame. **If you have a camera, a picture of your printer and whatever is
in front of it goes to your provider.** With a self-hosted gateway none of this
leaves your network; with a hosted provider it is subject to that provider's
data-retention policy.

**Other properties.** *Find models* only opens a browser URL — no model-supplied
link is ever followed by the slicer. When a model returns raw STL instead of a
shape spec, it is staged in a unique temp file, read through the normal importer,
and deleted immediately. The AI code paths do not touch slicing, profiles, or
project files.

---

## 5. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `No AI provider is configured…` | Provider is `Disabled`, or `OpenAI-compatible` was chosen without a **Gateway URL**. |
| `Authentication failed (401/403)` | Wrong key, or the key does not match the selected provider. |
| `Endpoint not found (404)` | Gateway URL is wrong. Give the API base (e.g. `https://host/v1/`), not a chat page. |
| `Rate limited (429)` | Provider quota. Wait, or use another key. |
| Test connection passes, generation fails | The endpoint is fine but the model is not capable. Run **Qualify for 3D**. |
| The model replies with prose | It cannot do structured tool calls. Switch to a mid/large instruct or code model. |
| No **AI** menu | Expected on Windows and Linux today — use the sidebar generator. |
| Generation works, but never reflects live printer state | Live telemetry needs a *selected* Physical Printer with a Moonraker host. |
| Anthropic never produces a shape | Known limitation — the forced tool call is not yet translated to the Anthropic tool schema. Use OpenAI or a compatible gateway meanwhile. See [PLAN.md](PLAN.md) §10. |
| The UI pauses briefly before generating | Machine context is gathered first, including a short probe of the printer host and camera (3 s connect timeout each). |
