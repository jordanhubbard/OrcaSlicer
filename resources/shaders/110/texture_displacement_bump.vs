#version 110

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

uniform mat4 volume_world_matrix;
// Clipping plane, x = min z, y = max z. Used by the FFF and SLA previews to clip with a top / bottom plane.
uniform vec2 z_range;
// Clipping plane - general orientation. Used by the SLA gizmo.
uniform vec4 clipping_plane;

attribute vec3  v_position;
// Per-vertex paint weight for the currently active texture-displacement layer, 0..1.
attribute float v_weight;

varying vec3  clipping_planes_dots;
varying vec4  model_pos;
varying vec4  world_pos;
varying float weight;

void main()
{
    model_pos = vec4(v_position, 1.0);
    world_pos = volume_world_matrix * model_pos;

    gl_Position = projection_matrix * view_model_matrix * model_pos;
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    weight = v_weight;
}
