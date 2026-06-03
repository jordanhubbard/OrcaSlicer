#pragma once
#include "IUiBackend.hpp"

namespace Slic3r { namespace GUI { namespace Automation {

// Real backend. Every public method marshals its work onto the GUI thread via
// wxGetApp().CallAfter + a std::future with a per-call timeout (error kErrGuiBusy on
// timeout). Walks the wxWindow tree, reads the ImGui item table, drives
// wxUIActionSimulator, captures screenshots.
class WxUiBackend : public IUiBackend {
public:
    explicit WxUiBackend(int gui_timeout_ms = 5000) : m_gui_timeout_ms(gui_timeout_ms) {}

    void     refresh_ui() override;
    UiNode   dump_tree(const DumpOptions& opts) override;
    AppState app_state() override;
    bool     click(const UiNode& node, MouseButton button, bool dbl,
                   const std::vector<KeyModifier>& modifiers) override;
    bool     type_text(const std::string& text) override;
    bool     send_keys(const std::vector<KeyChord>& chords) override;
    PngImage screenshot_window(const UiNode* target) override;
    int      open_files(const std::vector<std::string>& paths) override;
    int      select_view(const std::string& view) override;

private:
    int m_gui_timeout_ms;
};

}}} // namespace Slic3r::GUI::Automation
