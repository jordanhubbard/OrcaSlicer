#ifndef slic3r_TextureDisplacement_hpp_
#define slic3r_TextureDisplacement_hpp_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <array>

#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r {

class ModelVolume;

// Maximum number of simultaneous texture-displacement layers a single ModelVolume can hold.
// Each layer owns its own paint mask (ModelVolume::texture_displacement_facet(slot)), so this
// is also the number of independent EnforcerBlockerType selectors kept per volume.
static constexpr size_t TEXTURE_DISPLACEMENT_MAX_LAYERS = 8;

// One texture asset plus its projection/displacement parameters. Several layers may be painted
// onto overlapping areas of the same volume: they are applied in slot order (like layers in an
// image editor), each one displacing the surface that resulted from the layers before it. This
// is what "layered/blended" texture displacement means in this feature.
struct TextureDisplacementLayer
{
    // Index into ModelVolume::texture_displacement_facets, assigned once when the layer is
    // created. Not reused for the lifetime of the ModelVolume, so a deleted layer's slot simply
    // becomes unused rather than being handed to a different layer.
    int slot = -1;

    std::string name;
    // Path on the local filesystem the image was loaded from (informational; may be stale or
    // empty, e.g. after loading a .3mf on a different machine).
    std::string path;
    // Path inside the .3mf archive once saved (empty until the project is saved once).
    std::string path_in_3mf;
    // Raw encoded image bytes. Only 8-bit grayscale PNG is understood by decode_height_texture()
    // (libslic3r has no GUI image toolkit available); the GUI converts any imported image to that
    // format before storing it here, so the baking code never needs to depend on wxWidgets.
    std::shared_ptr<std::vector<unsigned char>> image_data;

    float depth_mm     = 0.4f; // maximum displacement along the surface normal, in mm
    float tiling_scale = 10.f; // size of one texture tile, in mm
    float rotation_deg = 0.f;
    Vec2f offset       = Vec2f::Zero();
    bool  invert       = false;

    bool empty() const { return !image_data || image_data->empty(); }

    template<class Archive> void save(Archive &ar) const
    {
        std::string blob = image_data ? std::string(image_data->begin(), image_data->end()) : std::string();
        ar(slot, name, path, path_in_3mf, blob, depth_mm, tiling_scale, rotation_deg, offset, invert);
    }
    template<class Archive> void load(Archive &ar)
    {
        std::string blob;
        ar(slot, name, path, path_in_3mf, blob, depth_mm, tiling_scale, rotation_deg, offset, invert);
        image_data = blob.empty() ? nullptr : std::make_shared<std::vector<unsigned char>>(blob.begin(), blob.end());
    }
};

// Decoded 8-bit grayscale height sample, independent of any GUI/OpenGL texture object so it can
// be evaluated from a background bake Job as well as from GUI-side preview code.
struct DecodedHeightTexture
{
    std::vector<uint8_t> pixels; // row-major, top-to-bottom, one byte per pixel
    int width  = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
    // Bilinearly sampled height in [0, 1] at a tiling (wrapping) normalized uv coordinate.
    float sample(const Vec2f &uv) const;
};

// Decode a layer's raw image bytes into sampleable grayscale height data. Returns an empty
// DecodedHeightTexture if image_data is empty or is not an 8-bit grayscale PNG.
DecodedHeightTexture decode_height_texture(const TextureDisplacementLayer &layer);

// Project a mesh-local position onto a layer's height texture, honouring its tiling scale,
// rotation and offset. Uses a simple dominant-axis planar projection of the given normal: proper
// seam-aware UV parametrization is a later-phase improvement (see project plan).
Vec2f project_texture_displacement_uv(const Vec3f &position, const Vec3f &normal, const TextureDisplacementLayer &layer);

// One paint mask (as stored by ModelVolume::texture_displacement_facets) per possible layer slot.
using TextureDisplacementFacetsData = std::array<TriangleSelector::TriangleSplittingData, TEXTURE_DISPLACEMENT_MAX_LAYERS>;

// Bake all painted texture-displacement layers into `base_mesh`'s geometry, restricted to the
// painted area(s) only (the rest of the mesh is left untouched). Layers are applied in slot
// order, each measuring its displacement from the surface left by the previous one. Returns the
// mesh unchanged if nothing is painted or no layer has a usable texture.
//
// Takes plain copied data rather than a ModelVolume reference so it is safe to call from a
// background thread (e.g. a bake Job's process() method) on a snapshot captured on the main
// thread, without touching the live Model concurrently with the UI.
//
// Known Phase 1-3 limitation: this does not attempt to remap texture-displacement paint data
// across topology-changing operations performed outside this gizmo (e.g. ModelObject::split(),
// mesh-boolean ops) the way TriangleSelector::remap_painting() does for the other paint channels.
// Such operations will silently drop any unbaked texture-displacement paint on the affected
// volume. This is an explicit extension point for a later phase, not an oversight.
indexed_triangle_set build_texture_displacement(const indexed_triangle_set              &base_mesh,
                                                 const std::vector<TextureDisplacementLayer> &layers,
                                                 const TextureDisplacementFacetsData         &facets_data);

// Convenience overload for main-thread callers: extracts the mesh/layers/paint data from `volume`
// and forwards to the overload above.
indexed_triangle_set build_texture_displacement(const ModelVolume &volume);

} // namespace Slic3r

#endif // slic3r_TextureDisplacement_hpp_
