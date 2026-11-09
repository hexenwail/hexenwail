uniform samplerCube u_texture0;
uniform vec3 u_wind3;
uniform float u_skyrot;
in vec3 v_dir;
in vec4 v_color;
out vec4 fragColor;
void main() {
    vec3 dir = normalize(v_dir);
    float cs = cos(u_skyrot), sn = sin(u_skyrot);
    dir.xz = vec2(dir.x * cs - dir.z * sn, dir.x * sn + dir.z * cs);
    fragColor = texture(u_texture0, dir + u_wind3) * v_color;
}
