#ifndef slic3r_GLGizmoTextureDisplacement_hpp_
#define slic3r_GLGizmoTextureDisplacement_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

// Paint-style gizmo that assigns one or more texture-displacement "layers" (see
// libslic3r/TextureDisplacement.hpp) to painted areas of a model, and can bake the result into
// real mesh geometry. See the project plan for the overall architecture; in short:
//  - each layer owns its own independent paint mask (ModelVolume::texture_displacement_facets),
//    reusing the same TriangleSelector/FacetsAnnotation machinery as every other paint gizmo --
//    only one layer is "active" (paintable) at a time, selected in the panel below;
//  - "Bake" runs build_texture_displacement() in a background job and commits the result exactly
//    like the Emboss/SVG "project on surface" gizmo does.
class GLGizmoTextureDisplacement : public GLGizmoPainterBase
{
public:
    GLGizmoTextureDisplacement(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Texture displacement painting"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Texture displacement painting"); }
    std::string get_action_snapshot_name() const override { return _u8L("Texture displacement editing"); }

    EnforcerBlockerType get_left_button_state_type() const override { return EnforcerBlockerType::ENFORCER; }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }

private:
    bool on_init() override;
    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void on_opening() override {}
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    // Phase 1 restricts the texture layer list to the first model-part volume of the current
    // object (the common single-volume case); multi-part objects only get texture layers on
    // their first part until a later phase. Returns nullptr if there is no model part.
    ModelVolume*       texture_volume();
    const ModelVolume* texture_volume() const;

    void add_texture_layer();
    void remove_texture_layer(int slot);
    void set_active_layer(int slot); // flushes the previous layer's edits, then reloads selectors
    void bake();

    // Which of the up to TEXTURE_DISPLACEMENT_MAX_LAYERS paint masks the brush currently writes
    // into. Always a valid slot index (0 by default) so the base class's per-volume selector
    // machinery always has something to work with, even before any texture has been added --
    // painting into a slot with no texture assigned is harmless, it just has no visible/bake
    // effect until a texture is added to that slot.
    int  m_active_layer_slot = 0;
    bool m_bake_in_progress  = false;

    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoTextureDisplacement_hpp_
