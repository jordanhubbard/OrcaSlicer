#version 140

// Fast, geometry-free preview of texture displacement: perturbs the *shading* normal from the
// height texture's local gradient (a bump map), faded out by the per-vertex paint weight. Used
// while the brush is actively dragging (see project plan, "Preview shaders" section); the true,
// exact result is what "Bake" produces via libslic3r/TextureDisplacement.cpp on the CPU.
//
// NOTE: the gradient-to-normal conversion below assumes the two planar-projection axes chosen by
// project_uv() are reasonably aligned with the surface here; it is a cheap approximation (no
// tangent/bitangent basis is reconstructed on the GPU), acceptable for a live preview but not
// intended to be pixel-exact versus the CPU-side bake.

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

#define INTENSITY_AMBIENT    0.3

const vec3 ZERO = vec3(0.0, 0.0, 0.0);

uniform vec4 uniform_color;
uniform bool volume_mirrored;

uniform mat4 view_model_matrix;
uniform mat3 view_normal_matrix;

uniform sampler2D height_tex;
uniform vec2      height_tex_texel; // (1/width, 1/height) of height_tex
uniform float      depth_mm;
uniform float      tiling_scale;
uniform float      rotation_rad;
uniform vec2       uv_offset;
uniform bool       invert;

in vec3  clipping_planes_dots;
in vec4  model_pos;
in vec4  world_pos;
in float weight;

out vec4 out_color;

vec2 project_uv(vec3 p, vec3 n)
{
    vec3 an = abs(n);
    vec2 planar = (an.x >= an.y && an.x >= an.z) ? p.yz : ((an.y >= an.x && an.y >= an.z) ? p.xz : p.xy);
    planar *= (tiling_scale > 1e-6) ? (1.0 / tiling_scale) : 1.0;
    float cs = cos(rotation_rad);
    float sn = sin(rotation_rad);
    return vec2(planar.x * cs - planar.y * sn, planar.x * sn + planar.y * cs) + uv_offset;
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec3 triangle_normal = normalize(cross(dFdx(model_pos.xyz), dFdy(model_pos.xyz)));
    if (volume_mirrored)
        triangle_normal = -triangle_normal;

    if (weight > 0.0) {
        vec2  uv = project_uv(model_pos.xyz, triangle_normal);
        float hL = texture(height_tex, uv - vec2(height_tex_texel.x, 0.0)).r;
        float hR = texture(height_tex, uv + vec2(height_tex_texel.x, 0.0)).r;
        float hD = texture(height_tex, uv - vec2(0.0, height_tex_texel.y)).r;
        float hU = texture(height_tex, uv + vec2(0.0, height_tex_texel.y)).r;
        float sign_mul = invert ? -1.0 : 1.0;
        vec3  bumped_normal = normalize(triangle_normal + sign_mul * depth_mm * vec3(hL - hR, hD - hU, 0.0));
        triangle_normal = normalize(mix(triangle_normal, bumped_normal, clamp(weight, 0.0, 1.0)));
    }

    vec3 eye_normal = normalize(view_normal_matrix * triangle_normal);
    float NdotL = max(dot(eye_normal, LIGHT_TOP_DIR), 0.0);

    vec2 intensity = vec2(0.0);
    intensity.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    vec3 position = (view_model_matrix * model_pos).xyz;
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position), reflect(-LIGHT_TOP_DIR, eye_normal)), 0.0), LIGHT_TOP_SHININESS);

    NdotL = max(dot(eye_normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    out_color = vec4(vec3(intensity.y) + uniform_color.rgb * intensity.x, uniform_color.a);
}
