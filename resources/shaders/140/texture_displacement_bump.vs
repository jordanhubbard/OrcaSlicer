#version 140

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

uniform mat4 volume_world_matrix;
// Clipping plane, x = min z, y = max z. Used by the FFF and SLA previews to clip with a top / bottom plane.
uniform vec2 z_range;
// Clipping plane - general orientation. Used by the SLA gizmo.
uniform vec4 clipping_plane;

in vec3  v_position;
// Per-vertex paint weight for the currently active texture-displacement layer, 0..1 (see
// TriangleSelectorWeighted... note: Phase 1 stores this as a plain enforced/not-enforced mask,
// so this is 0.0 or 1.0 today; a continuous value is a natural later-phase extension).
in float v_weight;

out vec3  clipping_planes_dots;
out vec4  model_pos;
out vec4  world_pos;
out float weight;

void main()
{
    model_pos = vec4(v_position, 1.0);
    world_pos = volume_world_matrix * model_pos;

    gl_Position = projection_matrix * view_model_matrix * model_pos;
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    weight = v_weight;
}
