#include "GLGizmoTextureDisplacement.hpp"

#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"

#include <glad/gl.h>
#include <algorithm>

namespace Slic3r::GUI {

GLGizmoTextureDisplacement::GLGizmoTextureDisplacement(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
{
}

bool GLGizmoTextureDisplacement::on_init()
{
    m_desc["cursor_size"]   = _L("Brush size");
    m_desc["circle"]        = _L("Circle");
    m_desc["sphere"]        = _L("Sphere");
    m_desc["add_texture"]   = _L("Add texture...");
    m_desc["remove_layer"]  = _L("Remove");
    m_desc["bake"]          = _L("Bake");
    m_desc["remove_all"]    = _L("Erase all");
    return true;
}

std::string GLGizmoTextureDisplacement::on_get_name() const
{
    return _u8L("Texture displacement");
}

void GLGizmoTextureDisplacement::on_shutdown()
{
    m_parent.toggle_model_objects_visibility(true);
}

PainterGizmoType GLGizmoTextureDisplacement::get_painter_type() const
{
    return PainterGizmoType::TEXTURE_DISPLACEMENT;
}

wxString GLGizmoTextureDisplacement::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    return shift_down ? _L("Erase texture displacement paint") : _L("Paint texture displacement");
}

void GLGizmoTextureDisplacement::render_painter_gizmo()
{
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    // Phase 1 reuses the standard paint-mask overlay (as every other paint gizmo does) to show
    // which area of the active layer is painted. A dedicated bump-map preview shader
    // (resources/shaders/*/texture_displacement_bump.*) is registered and ready to use, but
    // wiring it into this render path is deferred to a later phase -- see the project plan.
    // "Bake" already shows the true, real-geometry result, which is the most important preview.
    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

ModelVolume* GLGizmoTextureDisplacement::texture_volume()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return nullptr;
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part())
            return mv;
    return nullptr;
}

const ModelVolume* GLGizmoTextureDisplacement::texture_volume() const
{
    return const_cast<GLGizmoTextureDisplacement *>(this)->texture_volume();
}

void GLGizmoTextureDisplacement::update_model_object()
{
    bool         updated = false;
    ModelObject *mo      = m_c->selection_info()->model_object();
    int          idx     = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->texture_displacement_facet(m_active_layer_slot).set(*m_triangle_selectors[idx]);
    }

    if (updated) {
        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoTextureDisplacement::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        const TriangleMesh *mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        m_triangle_selectors.back()->deserialize(mv->texture_displacement_facet(m_active_layer_slot).get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
    }
}

void GLGizmoTextureDisplacement::set_active_layer(int slot)
{
    if (slot == m_active_layer_slot)
        return;
    // Flush edits made while the previous layer was active before switching what the selectors
    // reflect -- otherwise they would be silently lost.
    update_model_object();
    m_active_layer_slot = slot;
    update_from_model_object(false);
}

void GLGizmoTextureDisplacement::add_texture_layer()
{
    ModelVolume *mv = texture_volume();
    if (!mv)
        return;

    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS> used{};
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot >= 0 && size_t(l.slot) < used.size())
            used[size_t(l.slot)] = true;
    int free_slot = -1;
    for (size_t i = 0; i < used.size(); ++i)
        if (!used[i]) { free_slot = int(i); break; }
    if (free_slot < 0) {
        show_error(nullptr, _u8L("Maximum number of texture displacement layers reached."));
        return;
    }

    const wxString wildcard = "Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp";
    wxFileDialog   dialog(nullptr, _L("Choose a texture image (height map)"), wxEmptyString, wxEmptyString, wildcard,
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    const std::string path = into_u8(dialog.GetPath());

    wxImage image;
    if (!image.LoadFile(dialog.GetPath()) || !image.IsOk()) {
        show_error(nullptr, _u8L("Could not load the selected image."));
        return;
    }

    // libslic3r's PNG decoder (used by the baking code, which has no wxWidgets dependency) only
    // understands true 8-bit grayscale PNG. Convert and re-encode through Slic3r's own PNG writer
    // here rather than relying on wxImage's PNG encoder to pick a compatible color type.
    const wxImage gray = image.ConvertToGreyscale();
    const int     w    = gray.GetWidth();
    const int     h    = gray.GetHeight();
    if (w <= 0 || h <= 0) {
        show_error(nullptr, _u8L("The selected image is empty."));
        return;
    }
    std::vector<uint8_t> gray_pixels(size_t(w) * size_t(h));
    const unsigned char *rgb = gray.GetData();
    for (size_t i = 0; i < gray_pixels.size(); ++i)
        gray_pixels[i] = rgb[i * 3];

    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("orca_texdisp_%%%%%%%%.png");
    if (!Slic3r::png::write_gray_to_file(tmp_path.string(), size_t(w), size_t(h), gray_pixels)) {
        show_error(nullptr, _u8L("Failed to prepare the texture for use."));
        return;
    }
    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);

    if (bytes.empty()) {
        show_error(nullptr, _u8L("Failed to prepare the texture for use."));
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add texture displacement layer"), UndoRedo::SnapshotType::GizmoAction);

    TextureDisplacementLayer layer;
    layer.slot       = free_slot;
    layer.name       = boost::filesystem::path(path).filename().string();
    layer.path       = path;
    layer.image_data = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    mv->texture_displacement_layers.push_back(std::move(layer));

    set_active_layer(free_slot);
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::remove_texture_layer(int slot)
{
    ModelVolume *mv = texture_volume();
    if (!mv)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Remove texture displacement layer"), UndoRedo::SnapshotType::GizmoAction);

    auto &layers = mv->texture_displacement_layers;
    layers.erase(std::remove_if(layers.begin(), layers.end(),
                                 [slot](const TextureDisplacementLayer &l) { return l.slot == slot; }),
                 layers.end());
    if (slot >= 0 && slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
        mv->texture_displacement_facet(slot).reset();

    if (m_active_layer_slot == slot)
        update_from_model_object(false);

    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::bake()
{
    ModelVolume *mv = texture_volume();
    if (!mv || m_bake_in_progress)
        return;

    // Make sure the currently active layer's in-progress edits are flushed into the model before
    // baking, otherwise the most recent, not-yet-committed strokes would be silently skipped.
    update_model_object();

    if (!mv->is_texture_displacement_painted()) {
        show_error(nullptr, _u8L("Nothing is painted, there is nothing to bake."));
        return;
    }

    m_bake_in_progress = true;
    queue_texture_displacement_bake(*mv, [this]() { m_bake_in_progress = false; });
}

void GLGizmoTextureDisplacement::on_render_input_window(float x, float y, float bottom_limit)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return;
    ModelVolume *mv = texture_volume();

    const float approx_height = m_imgui->scaled(24.f);
    y = std::min(y, bottom_limit - approx_height);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    m_imgui->text(m_desc.at("cursor_size"));
    ImGui::SameLine();
    ImGui::PushItemWidth(m_imgui->scaled(7.f));
    m_imgui->slider_float("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f");

    bool is_circle = m_cursor_type == TriangleSelector::CursorType::CIRCLE;
    if (ImGui::RadioButton(m_desc.at("circle").ToUTF8().data(), is_circle))
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
    ImGui::SameLine();
    if (ImGui::RadioButton(m_desc.at("sphere").ToUTF8().data(), !is_circle))
        m_cursor_type = TriangleSelector::CursorType::SPHERE;

    ImGui::Separator();
    m_imgui->text(_L("Texture layers"));

    if (mv != nullptr) {
        std::vector<TextureDisplacementLayer *> ordered;
        for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
            ordered.push_back(&l);
        std::sort(ordered.begin(), ordered.end(), [](const auto *a, const auto *b) { return a->slot < b->slot; });

        for (TextureDisplacementLayer *layer : ordered) {
            ImGui::PushID(layer->slot);
            const bool is_active = layer->slot == m_active_layer_slot;
            if (ImGui::RadioButton("##active", is_active))
                set_active_layer(layer->slot);
            ImGui::SameLine();
            ImGui::Text("%s", layer->name.empty() ? "texture" : layer->name.c_str());
            ImGui::SameLine();
            if (m_imgui->button(m_desc.at("remove_layer")))
                remove_texture_layer(layer->slot);

            ImGui::PushItemWidth(m_imgui->scaled(7.f));
            m_imgui->slider_float(_u8L("Depth (mm)"), &layer->depth_mm, 0.02f, 5.f, "%.2f");
            m_imgui->slider_float(_u8L("Tile size (mm)"), &layer->tiling_scale, 0.5f, 100.f, "%.1f");
            m_imgui->slider_float(_u8L("Rotation"), &layer->rotation_deg, 0.f, 360.f, "%.0f");
            ImGui::PopItemWidth();
            ImGui::Checkbox(_u8L("Invert").c_str(), &layer->invert);
            ImGui::PopID();
            ImGui::Separator();
        }
    }

    if (m_imgui->button(m_desc.at("add_texture")))
        add_texture_layer();

    ImGui::Separator();

    m_imgui->disabled_begin(mv == nullptr || !mv->is_texture_displacement_painted());
    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset texture displacement selection"), UndoRedo::SnapshotType::GizmoAction);
        int idx = -1;
        for (ModelVolume *v : mo->volumes)
            if (v->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data();
            }
        update_model_object();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();

    ImGui::SameLine();
    m_imgui->disabled_begin(m_bake_in_progress || mv == nullptr || !mv->is_texture_displacement_painted());
    if (m_imgui->button(m_bake_in_progress ? _L("Baking...") : m_desc.at("bake")))
        bake();
    m_imgui->disabled_end();

    ImGui::Separator();
    if (m_imgui->button(_L("Done")))
        m_parent.reset_all_gizmos();

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
