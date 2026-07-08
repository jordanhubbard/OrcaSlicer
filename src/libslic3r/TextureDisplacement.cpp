#include "TextureDisplacement.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <optional>

#include "Model.hpp"
#include "PNGReadWrite.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r {

float DecodedHeightTexture::sample(const Vec2f &uv) const
{
    if (empty())
        return 0.f;

    auto wrap01 = [](float x) {
        x = std::fmod(x, 1.f);
        return x < 0.f ? x + 1.f : x;
    };

    const float fx = wrap01(uv.x()) * float(width);
    const float fy = wrap01(uv.y()) * float(height);
    int x0 = int(std::floor(fx)) % width;
    int y0 = int(std::floor(fy)) % height;
    if (x0 < 0) x0 += width;
    if (y0 < 0) y0 += height;
    const int   x1 = (x0 + 1) % width;
    const int   y1 = (y0 + 1) % height;
    const float tx = fx - std::floor(fx);
    const float ty = fy - std::floor(fy);

    auto at = [this](int x, int y) { return float(pixels[size_t(y) * size_t(width) + size_t(x)]) / 255.f; };
    const float top    = at(x0, y0) * (1.f - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.f - tx) + at(x1, y1) * tx;
    return top * (1.f - ty) + bottom * ty;
}

DecodedHeightTexture decode_height_texture(const TextureDisplacementLayer &layer)
{
    DecodedHeightTexture result;
    if (layer.empty())
        return result;

    const png::ReadBuf rbuf{ layer.image_data->data(), layer.image_data->size() };
    if (!png::is_png(rbuf))
        // Only 8-bit grayscale PNG height maps are supported. The GUI is responsible for
        // converting any imported image (jpg, color png, ...) to that format on import, so this
        // code never needs a dependency on wxWidgets/libjpeg to decode arbitrary user images.
        return result;

    png::ImageGreyscale img;
    if (!png::decode_png(rbuf, img) || img.cols == 0 || img.rows == 0)
        return result;

    result.width  = int(img.cols);
    result.height = int(img.rows);
    result.pixels = std::move(img.buf);
    return result;
}

Vec2f project_texture_displacement_uv(const Vec3f &position, const Vec3f &normal, const TextureDisplacementLayer &layer)
{
    // Planar-project onto the two axes orthogonal to the dominant component of `normal`. Using a
    // single projection axis for an entire patch (rather than per-vertex normals) avoids visible
    // seams where the local normal direction changes. Proper seam-aware UV parametrization is a
    // later-phase improvement.
    const Vec3f n = normal.cwiseAbs();
    Vec2f       planar;
    if (n.x() >= n.y() && n.x() >= n.z())
        planar = Vec2f(position.y(), position.z());
    else if (n.y() >= n.x() && n.y() >= n.z())
        planar = Vec2f(position.x(), position.z());
    else
        planar = Vec2f(position.x(), position.y());

    const float scale = (layer.tiling_scale > 1e-6f) ? (1.f / layer.tiling_scale) : 1.f;
    planar *= scale;

    const float rad = layer.rotation_deg * float(M_PI) / 180.f;
    const float cs  = std::cos(rad);
    const float sn  = std::sin(rad);
    const Vec2f rotated(planar.x() * cs - planar.y() * sn, planar.x() * sn + planar.y() * cs);

    return rotated + layer.offset;
}

// Area-weighted vertex normals computed from the patch's own (pre-displacement) connectivity.
// Since the patch's vertices have not moved yet at the point this is called, these are identical
// to the surface normals of the mesh the patch was extracted from.
static std::vector<Vec3f> texture_displacement_vertex_normals(const indexed_triangle_set &its)
{
    std::vector<Vec3f> normals(its.vertices.size(), Vec3f::Zero());
    for (const stl_triangle_vertex_indices &tri : its.indices) {
        const Vec3f &a = its.vertices[tri[0]];
        const Vec3f &b = its.vertices[tri[1]];
        const Vec3f &c = its.vertices[tri[2]];
        // Cross product magnitude is twice the face area, so this naturally area-weights the
        // contribution of each incident face to its vertices.
        const Vec3f area_weighted_normal = (b - a).cross(c - a);
        normals[tri[0]] += area_weighted_normal;
        normals[tri[1]] += area_weighted_normal;
        normals[tri[2]] += area_weighted_normal;
    }
    for (Vec3f &n : normals) {
        const float len = n.norm();
        n = (len > 1e-8f) ? Vec3f(n / len) : Vec3f::UnitZ();
    }
    return normals;
}

indexed_triangle_set build_texture_displacement(const indexed_triangle_set                  &base_mesh,
                                                 const std::vector<TextureDisplacementLayer> &layers,
                                                 const TextureDisplacementFacetsData         &facets_data)
{
    const indexed_triangle_set original_mesh = base_mesh;
    indexed_triangle_set       working_mesh   = original_mesh;
    bool                       mesh_modified  = false;

    // Layers behave like stacked image-editor layers: applied in slot order, each one sculpting
    // the surface left by the layers before it. This is what makes overlapping layers "blend".
    std::vector<const TextureDisplacementLayer *> ordered_layers;
    for (const TextureDisplacementLayer &layer : layers)
        if (!layer.empty() && layer.slot >= 0 && size_t(layer.slot) < TEXTURE_DISPLACEMENT_MAX_LAYERS)
            ordered_layers.push_back(&layer);
    std::sort(ordered_layers.begin(), ordered_layers.end(),
               [](const TextureDisplacementLayer *a, const TextureDisplacementLayer *b) { return a->slot < b->slot; });

    for (const TextureDisplacementLayer *layer : ordered_layers) {
        const TriangleSelector::TriangleSplittingData &stored_data = facets_data[size_t(layer->slot)];
        if (stored_data.triangles_to_split.empty())
            continue;

        const DecodedHeightTexture height = decode_height_texture(*layer);
        if (height.empty())
            continue;

        TriangleSelector::TriangleSplittingData data = stored_data;
        if (mesh_modified) {
            // The stored paint mask is relative to the volume's original (unbaked) mesh; remap it
            // onto the working mesh, which earlier layers may have already displaced/subdivided.
            data = TriangleSelector::remap_painting(original_mesh, data, working_mesh, Transform3d::Identity(),
                                                     std::optional<std::reference_wrapper<const TriangleSelector::TriangleSplittingData>>{});
            if (data.bitstream.empty())
                continue;
        }

        const TriangleMesh    selector_mesh(working_mesh);
        TriangleSelector       selector(selector_mesh);
        selector.deserialize(data, false);

        const indexed_triangle_set patch = selector.get_facets_strict(EnforcerBlockerType::ENFORCER);
        if (patch.indices.empty())
            continue;
        // get_facets_strict() returns the *entire* mesh's vertex array regardless of which state
        // was asked for (only the returned triangle indices are filtered by state) -- so `patch`
        // and `rest` share the exact same vertex indexing. That is what lets the weld below be a
        // plain index check instead of a position-based lookup.
        indexed_triangle_set rest = selector.get_facets_strict(EnforcerBlockerType::NONE);
        assert(rest.vertices.size() == patch.vertices.size());

        // A vertex that is also used by at least one *unpainted* triangle is a boundary vertex:
        // its final position is ambiguous (it belongs to both the painted and untouched surface),
        // so it must never be displaced -- otherwise the baked patch would tear away from the
        // rest of the mesh. Only vertices used exclusively by painted triangles ("interior" to the
        // patch) are eligible for displacement. This is what keeps the bake seamless without any
        // remeshing/hole-filling at the boundary.
        std::vector<bool> is_boundary_vertex(rest.vertices.size(), false);
        for (const stl_triangle_vertex_indices &tri : rest.indices)
            for (int i = 0; i < 3; ++i)
                is_boundary_vertex[tri[i]] = true;

        const std::vector<Vec3f> vertex_normals = texture_displacement_vertex_normals(patch);

        Vec3f average_normal = Vec3f::Zero();
        for (const stl_triangle_vertex_indices &tri : patch.indices)
            for (int i = 0; i < 3; ++i)
                average_normal += vertex_normals[tri[i]];
        average_normal = (average_normal.norm() > 1e-8f) ? Vec3f(average_normal.normalized()) : Vec3f::UnitZ();

        const float          sign = layer->invert ? -1.f : 1.f;
        std::vector<int>     interior_vertex_remap(rest.vertices.size(), -1);
        indexed_triangle_set working_mesh_next = std::move(rest);
        for (const stl_triangle_vertex_indices &tri : patch.indices) {
            stl_triangle_vertex_indices final_tri;
            for (int i = 0; i < 3; ++i) {
                const int vi = tri[i];
                if (is_boundary_vertex[vi]) {
                    // Shared with the untouched surface: reuse the existing, undisplaced vertex.
                    final_tri[i] = vi;
                    continue;
                }
                if (interior_vertex_remap[vi] < 0) {
                    const Vec2f uv        = project_texture_displacement_uv(patch.vertices[vi], average_normal, *layer);
                    const float h         = height.sample(uv);
                    const Vec3f displaced = patch.vertices[vi] + vertex_normals[vi] * (h * layer->depth_mm * sign);
                    interior_vertex_remap[vi] = int(working_mesh_next.vertices.size());
                    working_mesh_next.vertices.push_back(displaced);
                }
                final_tri[i] = interior_vertex_remap[vi];
            }
            working_mesh_next.indices.push_back(final_tri);
        }

        working_mesh  = std::move(working_mesh_next);
        mesh_modified = true;
    }

    if (mesh_modified)
        its_compactify_vertices(working_mesh);

    return working_mesh;
}

indexed_triangle_set build_texture_displacement(const ModelVolume &volume)
{
    TextureDisplacementFacetsData facets_data;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        facets_data[size_t(i)] = volume.texture_displacement_facet(i).get_data();

    return build_texture_displacement(volume.mesh().its, volume.texture_displacement_layers, facets_data);
}

} // namespace Slic3r
