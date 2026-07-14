#include "AISlicerDialog.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "GLCanvas3D.hpp"
#include "Widgets/Button.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "slic3r/Utils/AIProvider.hpp"
#include "slic3r/Utils/AIPrinterContext.hpp"
#include "slic3r/Utils/AIShapeGen.hpp"

namespace Slic3r { namespace GUI {

// Bed extents (mm) from the active machine config, for the printable-fit check.
static void compute_bed_dims(const DynamicPrintConfig &cfg, double &x, double &y, double &z)
{
    z = (cfg.option("printable_height") != nullptr) ? cfg.opt_float("printable_height") : 250.0;
    x = y = 220.0;
    if (auto *pa = cfg.option<ConfigOptionPoints>("printable_area")) {
        double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
        for (const Vec2d &p : pa->values) {
            minx = std::min(minx, p.x()); maxx = std::max(maxx, p.x());
            miny = std::min(miny, p.y()); maxy = std::max(maxy, p.y());
        }
        if (maxx > minx && maxy > miny) { x = maxx - minx; y = maxy - miny; }
    }
}

AISlicerDialog::AISlicerDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("AI"), wxDefaultPosition,
                wxSize(52 * wxGetApp().em_unit(), -1), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    auto *nb = new wxNotebook(this, wxID_ANY);
    nb->AddPage(build_generate_tab(nb), _L("Generate shape"), true);

    auto *close_btn = new Button(this, _L("Close"));
    close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    auto *btn_row = new wxBoxSizer(wxHORIZONTAL);
    btn_row->AddStretchSpacer(1);
    btn_row->Add(close_btn, 0);

    auto *top = new wxBoxSizer(wxVERTICAL);
    top->Add(nb,      1, wxEXPAND | wxALL, FromDIP(12));
    top->Add(btn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    SetSizer(top);
    top->SetSizeHints(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

wxWindow *AISlicerDialog::build_generate_tab(wxNotebook *nb)
{
    auto *panel = new wxPanel(nb, wxID_ANY);
    auto *s = new wxBoxSizer(wxVERTICAL);

    s->Add(new wxStaticText(panel, wxID_ANY, _L("Describe the object you want to create:")),
           0, wxALL, FromDIP(10));

    m_prompt = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition,
                              wxSize(FromDIP(420), FromDIP(110)), wxTE_MULTILINE);
    s->Add(m_prompt, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    s->Add(new wxStaticText(panel, wxID_ANY,
               _L("Your printer's settings and live sensor data (bed, nozzle, material, temperatures, "
                  "and a camera frame when available) are sent with each request, so the shape fits the machine.")),
           0, wxALL, FromDIP(10));

    m_generate_btn = new Button(panel, _L("Generate"));
    m_generate_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_generate(); });
    s->Add(m_generate_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_status = new wxStaticText(panel, wxID_ANY, "");
    s->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    panel->SetSizer(s);
    return panel;
}

void AISlicerDialog::on_generate()
{
    const wxString promptw = m_prompt->GetValue();
    if (promptw.IsEmpty()) { set_status(_L("Enter a description first."), true); return; }

    auto *ac = wxGetApp().app_config;
    std::unique_ptr<AIProvider> provider(
        ac ? AIProvider::create(AIProvider::config_from_app_config(*ac)) : nullptr);
    if (! provider) {
        set_status(_L("No AI provider is configured. Open AI Settings and register a provider and API key first."), true);
        return;
    }

    // Assemble the request: shape-spec contract + machine context as system, the
    // user's description (and a camera frame, if any) as the user turn.
    AIPrinterContext ctx = AIPrinterContext::gather();

    std::vector<AIMessage> messages;
    AIMessage sys;
    sys.role    = "system";
    sys.content = ai_shape_spec_instructions() +
                  "\n\nPrinter context (respect these constraints; never exceed the build volume):\n" +
                  ctx.to_prompt();
    messages.push_back(std::move(sys));

    AIMessage user;
    user.role    = "user";
    user.content = into_u8(promptw);
    if (ctx.has_image())
        user.images.push_back(ctx.to_image());
    messages.push_back(std::move(user));

    set_status(_L("Generating..."));
    AIResponse resp;
    {
        wxBusyCursor wait; // the LLM round-trip briefly blocks the UI
        resp = provider->chat(messages);
    }
    if (! resp.ok) { set_status(_L("AI request failed: ") + from_u8(resp.error), true); return; }

    Model result;
    std::string error;
    const std::string name = into_u8(promptw).substr(0, 40);
    if (! ai_build_model_from_response(resp.content, name, result, error)) {
        set_status(_L("Could not build a model from the AI response: ") + from_u8(error), true);
        return;
    }
    if (result.objects.empty()) { set_status(_L("The AI did not return a usable shape."), true); return; }

    // Printable-fit check (informational — we still add it so the user can rescale).
    double bx = 220, by = 220, bz = 250;
    if (auto *bundle = wxGetApp().preset_bundle)
        compute_bed_dims(bundle->full_config(), bx, by, bz);
    std::string warn;
    const bool fits = ai_model_fits_bed(result, bx, by, bz, warn);

    // Inject on the main thread (we are on it): mutate the Plater model, then
    // register + select via the object list.
    Plater *plater = wxGetApp().plater();
    plater->take_snapshot(into_u8(_L("Add AI generated object")));
    Model &model = plater->model();
    std::vector<size_t> idxs;
    for (const ModelObject *src : result.objects) {
        ModelObject *dst = model.add_object(*src);
        dst->ensure_on_bed();
        idxs.push_back(model.objects.size() - 1);
    }
    wxGetApp().obj_list()->paste_objects_into_list(idxs);
    if (auto *canvas = plater->get_view3D_canvas3D())
        canvas->reload_scene(true);

    if (fits)
        set_status(_L("Added the generated object to the plate."));
    else
        set_status(from_u8(warn) + " " + _L("Added anyway — scale it to fit."), true);
}

void AISlicerDialog::set_status(const wxString &msg, bool error)
{
    if (m_status == nullptr)
        return;
    m_status->SetForegroundColour(error ? wxColour(0xC0, 0x30, 0x30) : wxColour(0x30, 0x80, 0x30));
    m_status->SetLabel(msg);
    Layout();
}

void AISlicerDialog::on_dpi_changed(const wxRect & /*suggested_rect*/)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
