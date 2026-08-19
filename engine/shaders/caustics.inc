float Caustics(vec2 p, float t) {
    vec2 q1 = p * 0.06 + vec2(t * 0.42, t * 0.31);
    vec2 q2 = p * 0.05 - vec2(t * 0.27, t * 0.49);
    float c1 = sin(q1.x + sin(q1.y + t));
    float c2 = sin(q2.y * 1.3 + sin(q2.x * 0.9 + t * 1.1));
    return pow(max(c1 * c2 * 0.5 + 0.5, 0.0), 4.0);
}
