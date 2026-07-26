#ifndef INCLUDE_util_Math_glsl
#define INCLUDE_util_Math_glsl a

#define PI 3.14159265358979323846
#define PI_2 (2.0 * PI)
#define rcp(x) (1.0 / (x))
#define saturate(x) clamp(x, 0.0, 1.0)

float mmin3(vec3 v) { return min(min(v.x, v.y), v.z); }
float mmax3(vec3 v) { return max(max(v.x, v.y), v.z); }
float linearStep(float edge0, float edge1, float x) { return saturate((x - edge0) / (edge1 - edge0)); }
vec3 linearStep(float edge0, float edge1, vec3 x) { return saturate((x - edge0) / (edge1 - edge0)); }
float pow2(float x) { return x * x; }
float safeRcp(float x) { return x <= 0.0 ? 0.0 : rcp(x); }

vec3 softMin(vec3 x, vec3 maxV) {
    vec3 phiX = x - maxV / 2.0;
    vec3 phi = maxV / (1.0 + exp((-4.0 * phiX) / maxV));
    return min(x, phi);
}

#endif
