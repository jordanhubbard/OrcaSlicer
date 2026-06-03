#include "WxUiBackend.hpp"
#include "AutomationRegistry.hpp"
#include "ImGuiItemTable.hpp"
#include "JsonRpcDispatcher.hpp"   // for kErrGuiBusy and friends
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"  // get_current_canvas3D() for app.state
#include "libslic3r/Model.hpp"

#include <wx/window.h>
#include <wx/toplevel.h>
#include <wx/dialog.h>     // wxDialog::IsModal
#include <wx/glcanvas.h>   // wxGLCanvas -> wxWindow* conversion
#include <wx/textctrl.h>   // wxTextEntry
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/uiaction.h>   // wxUIActionSimulator (synthetic mouse/keyboard)
#include <wx/dcscreen.h>   // wxScreenDC
#include <wx/dcmemory.h>   // wxMemoryDC
#include <wx/mstream.h>    // wxMemoryOutputStream

#include <cctype>          // std::toupper
#include <cstdlib>         // std::atoi
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <type_traits>

namespace Slic3r { namespace GUI { namespace Automation {

// Run `fn` on the GUI thread, block until it returns or the timeout elapses.
// Throws AutomationError(kErrGuiBusy) on timeout. std::promise is move-only, so we
// hold it via shared_ptr to satisfy CallAfter's copyable-functor requirement.
template <class Fn>
static auto run_on_gui(int timeout_ms, Fn&& fn) -> decltype(fn()) {
    using R = decltype(fn());
    auto prom = std::make_shared<std::promise<R>>();
    auto fut  = prom->get_future();
    wxGetApp().CallAfter([prom, fn = std::forward<Fn>(fn)]() mutable {
        try {
            if constexpr (std::is_void_v<R>) { fn(); prom->set_value(); }
            else { prom->set_value(fn()); }
        } catch (...) { prom->set_exception(std::current_exception()); }
    });
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
        throw AutomationError(kErrGuiBusy, "GUI thread timed out");
    return fut.get();
}

namespace {
std::string wx_class_name(const wxWindow* w) {
    const wxClassInfo* ci = w->GetClassInfo();
    std::string name = ci ? std::string(wxString(ci->GetClassName()).ToUTF8()) : "wxWindow";
    if (name.rfind("wx", 0) == 0 && name.size() > 2) name = name.substr(2);
    return name;
}

std::string wx_value_of(wxWindow* w, bool& has_value) {
    has_value = false;
    if (auto* tc = dynamic_cast<wxTextEntry*>(w))   { has_value = true; return std::string(tc->GetValue().ToUTF8()); }
    if (auto* ch = dynamic_cast<wxChoice*>(w))      { has_value = true; return std::string(ch->GetStringSelection().ToUTF8()); }
    if (auto* cb = dynamic_cast<wxCheckBox*>(w))    { has_value = true; return cb->GetValue() ? "true" : "false"; }
    return {};
}

void build_node(wxWindow* w, UiNode& node, const std::string& parent_path,
                int sibling_index, const DumpOptions& opts, int depth) {
    node.backend = BackendKind::Wx;
    node.klass   = wx_class_name(w);
    node.id      = automation_id_of(w);
    node.path    = parent_path.empty()
                       ? node.klass
                       : parent_path + "/" + node.klass + "[" + std::to_string(sibling_index) + "]";
    node.label   = std::string(w->GetLabel().ToUTF8());
    node.enabled = w->IsEnabled();
    node.visible = w->IsShownOnScreen();
    node.value   = wx_value_of(w, node.has_value);
    node.handle  = reinterpret_cast<std::uint64_t>(w);
    const wxRect r = w->GetScreenRect();
    node.rect = { r.x, r.y, r.width, r.height };

    if (opts.max_depth >= 0 && depth >= opts.max_depth) return;
    int idx = 0;
    for (wxWindow* child : w->GetChildren()) {
        if (opts.visible_only && !child->IsShownOnScreen()) { ++idx; continue; }
        UiNode cn;
        build_node(child, cn, node.path, idx, opts, depth + 1);
        node.children.push_back(std::move(cn));
        ++idx;
    }
}

// Map recorded ImGui items (display coords) to screen coords using the 3D canvas
// client origin + DPI scale, then append them as flat children under the root.
void append_imgui_nodes(UiNode& root) {
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr) return;
    GLCanvas3D* canvas3d = plater->get_current_canvas3D();
    if (canvas3d == nullptr) return;
    wxWindow* canvas = canvas3d->get_wxglcanvas();
    if (canvas == nullptr) return;
    const wxPoint origin = canvas->ClientToScreen(wxPoint(0, 0));
    double scale = canvas->GetContentScaleFactor();
    if (scale <= 0.0) scale = 1.0;
    const auto frame = ImGuiItemTable::instance().snapshot();
    for (const auto& it : frame.items) {
        UiNode n;
        n.backend   = BackendKind::ImGui;
        n.klass     = it.type;
        n.label     = it.label;
        n.path      = "ImGui/" + it.window_name + "/" + it.label;
        n.id        = n.path; // imgui items use their path as id in v1
        n.enabled   = it.enabled;
        n.visible   = true;
        n.has_value = it.has_value;
        n.value     = it.value;
        n.rect = { origin.x + int(it.x / scale), origin.y + int(it.y / scale),
                   int(it.w / scale), int(it.h / scale) };
        root.children.push_back(std::move(n));
    }
}
} // namespace

void WxUiBackend::refresh_ui() {
    run_on_gui(m_gui_timeout_ms, [] {
        // Force a fresh ImGui frame so transient items are recorded, then flush
        // pending events so the latest frame is the one we read.
        if (Plater* p = wxGetApp().plater()) {
            if (GLCanvas3D* canvas = p->get_current_canvas3D()) {
                canvas->set_as_dirty();
                canvas->render();
            }
        }
        wxGetApp().Yield();
    });
}

UiNode WxUiBackend::dump_tree(const DumpOptions& opts) {
    return run_on_gui(m_gui_timeout_ms, [&opts]() -> UiNode {
        wxWindow* root_win = nullptr;
        if (opts.root) root_win = window_for_automation_id(*opts.root);
        if (root_win == nullptr)
            root_win = static_cast<wxWindow*>(wxGetApp().mainframe);
        UiNode root;
        if (root_win) build_node(root_win, root, {}, 0, opts, 0);
        if (opts.include_imgui) append_imgui_nodes(root);
        return root;
    });
}

AppState WxUiBackend::app_state() {
    return run_on_gui(m_gui_timeout_ms, []() -> AppState {
        AppState s;
        MainFrame* mf = wxGetApp().mainframe;
        Plater*    p  = wxGetApp().plater();
        if (mf) {
            // best-effort: MainFrame has no public getter for the selected top tab
            // (m_tabpanel is private), so report the frame title for now.
            s.active_tab = std::string(mf->GetTitle().ToUTF8());
            s.foreground = mf->IsActive();
        }
        if (p) {
            s.project_loaded = !p->model().objects.empty();
            s.slicing        = p->is_background_process_slicing();
        }
        if (wxWindow* top = wxGetActiveWindow())
            if (auto* dlg = dynamic_cast<wxDialog*>(top))
                if (dlg != static_cast<wxWindow*>(mf) && dlg->IsModal())
                    s.modal_dialog = std::string(dlg->GetTitle().ToUTF8());
        return s;
    });
}

// ---------------------------------------------------------------------------
// Input helpers (anonymous namespace)
// ---------------------------------------------------------------------------
namespace {
// Map a normalized key name (single char, or "enter"/"tab"/"f5"/...) to a wx
// keycode. Returns 0 when unmapped (caller skips it).
long wx_keycode(const std::string& key) {
    if (key.size() == 1) return (long)std::toupper((unsigned char)key[0]);
    if (key == "enter" || key == "return") return WXK_RETURN;
    if (key == "tab")    return WXK_TAB;
    if (key == "esc" || key == "escape") return WXK_ESCAPE;
    if (key == "space")  return WXK_SPACE;
    if (key == "delete") return WXK_DELETE;
    if (key == "backspace") return WXK_BACK;
    if (key.size() >= 2 && (key[0] == 'f' || key[0] == 'F')) {
        int n = std::atoi(key.c_str() + 1);
        if (n >= 1 && n <= 12) return WXK_F1 + (n - 1);
    }
    return 0;
}

// Press (down==true) or release (down==false) the modifier keys. Cmd maps to
// Ctrl on the platforms we drive here.
void apply_modifiers_down(wxUIActionSimulator& sim,
                          const std::vector<KeyModifier>& mods, bool down) {
    for (KeyModifier m : mods) {
        long code = (m == KeyModifier::Ctrl)  ? WXK_CONTROL :
                    (m == KeyModifier::Shift) ? WXK_SHIFT :
                    (m == KeyModifier::Alt)   ? WXK_ALT : WXK_CONTROL; // Cmd~Ctrl
        if (down) sim.KeyDown((int)code); else sim.KeyUp((int)code);
    }
}
} // namespace

bool WxUiBackend::click(const UiNode& node, MouseButton button, bool dbl,
                        const std::vector<KeyModifier>& modifiers) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> bool {
        // Raise/focus the owning top-level window so OS input lands on it.
        if (auto* w = reinterpret_cast<wxWindow*>(node.handle)) {
            if (wxWindow* tlw = wxGetTopLevelParent(w)) tlw->Raise();
            w->SetFocus();
        }
        const int cx = node.rect.x + node.rect.w / 2;
        const int cy = node.rect.y + node.rect.h / 2;
        wxUIActionSimulator sim;
        sim.MouseMove(cx, cy);
        apply_modifiers_down(sim, modifiers, true);
        const int b = (button == MouseButton::Right)  ? wxMOUSE_BTN_RIGHT :
                      (button == MouseButton::Middle) ? wxMOUSE_BTN_MIDDLE :
                                                        wxMOUSE_BTN_LEFT;
        if (dbl) sim.MouseDblClick(b); else sim.MouseClick(b);
        apply_modifiers_down(sim, modifiers, false);
        return true;
    });
}

bool WxUiBackend::type_text(const std::string& text) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> bool {
        wxUIActionSimulator sim;
        // wxUIActionSimulator::Text takes a const char*; `text` is already UTF-8.
        sim.Text(text.c_str());
        return true;
    });
}

bool WxUiBackend::send_keys(const std::vector<KeyChord>& chords) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> bool {
        wxUIActionSimulator sim;
        for (const KeyChord& c : chords) {
            const long code = wx_keycode(c.key);
            if (code == 0) continue;
            apply_modifiers_down(sim, c.modifiers, true);
            sim.Char((int)code);
            apply_modifiers_down(sim, c.modifiers, false);
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Screenshot helpers (anonymous namespace)
// ---------------------------------------------------------------------------
namespace {
PngImage wximage_to_png(const wxImage& image) {
    wxMemoryOutputStream mem;
    if (!image.SaveFile(mem, wxBITMAP_TYPE_PNG))
        throw AutomationError(kErrScreenshotFail, "PNG encode failed");
    PngImage out;
    out.width  = image.GetWidth();
    out.height = image.GetHeight();
    const size_t n = mem.GetSize();
    out.png.resize(n);
    mem.CopyTo(out.png.data(), n);
    return out;
}

} // namespace

PngImage WxUiBackend::screenshot_window(const UiNode* target) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> PngImage {
        wxWindow* win = target ? reinterpret_cast<wxWindow*>(target->handle)
                               : static_cast<wxWindow*>(wxGetApp().mainframe);
        if (win == nullptr)
            throw AutomationError(kErrScreenshotFail, "no window to capture");
        const wxSize sz = win->GetClientSize();
        if (sz.x <= 0 || sz.y <= 0)
            throw AutomationError(kErrScreenshotFail, "window has no client area");
        // Capture from the on-screen (DWM-composited) framebuffer rather than the
        // window's own client DC. A parent client DC clips out child HWNDs, so all of
        // OrcaSlicer's custom child-window controls (sidebar buttons/combos/panels) and
        // the GL canvas come back as uninitialized (black) bitmap memory. wxScreenDC
        // reads the composited desktop, which includes every child window, the OpenGL
        // surface, and ImGui overlays — pixel-perfect to what the user sees.
        //
        // Requirement: the window must be visible and unobscured (see doc/automation.md
        // §platform caveats); the backend raises it before injecting input anyway.
        // HiDPI note: GetClientSize is in logical units while wxScreenDC is in physical
        // pixels; on per-monitor-DPI setups the captured size may differ from the logical
        // client size (documented caveat, acceptable for v1).
        win->Raise();
        const wxPoint origin = win->ClientToScreen(wxPoint(0, 0));
        wxBitmap bmp(sz.x, sz.y);
        wxScreenDC sdc;
        wxMemoryDC mdc(bmp);
        mdc.Blit(0, 0, sz.x, sz.y, &sdc, origin.x, origin.y);
        mdc.SelectObject(wxNullBitmap);
        return wximage_to_png(bmp.ConvertToImage());
    });
}

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

int WxUiBackend::select_view(const std::string& view) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> int {
        MainFrame* mainframe = wxGetApp().mainframe;
        if (mainframe == nullptr)
            throw AutomationError(kErrNotFound, "no main frame to select a view in");
        const int index = mainframe->select_tab_by_name(view);
        if (index < 0)
            throw AutomationError(kErrNotFound,
                                  std::string("view not available: ") + view);
        return index;
    });
}

}}} // namespace Slic3r::GUI::Automation
