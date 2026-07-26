void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uimg_outputTex);
    if (any(greaterThanEqual(pixel, size))) return;

    vec3 color = texture(usam_inputTex, (vec2(pixel) + 0.5) / vec2(size)).rgb;
#ifdef DRT_BENCH_SDR
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
#endif
    imageStore(uimg_outputTex, pixel, vec4(color, 1.0));
}
