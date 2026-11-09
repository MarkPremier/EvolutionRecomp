#include <cassert>
#include "../../../xboxrecomp_fork/src/kernel/nv2a_gpu_d3d11.cpp"

/* Host/game hooks outside the shader cache are never used by these tests. */
extern "C" {
void (*g_xbox_path_hook)(const char *) = nullptr;
int nv2a_pb_exec_batch_spans(float) { assert(false); return 0; }
void xbox_Nv2aAcknowledgeHandshakes(void) { assert(false); }
void xbox_CloakMarker(const char *) { assert(false); }
void xbox_FramebufferSize(uint32_t *, uint32_t *) { assert(false); }
}

int main()
{
    _putenv_s("RECOMP_SHADER_ASYNC", "0");
    _putenv_s("RECOMP_SHADER_CACHE", "0");
    D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &device, &level, &context)));
    ComPtr<ID3DBlob> code, errors;
    const char source[] = "float4 vs_guest(float4 p:TEXCOORD0):SV_Position{return p;}";
    assert(SUCCEEDED(D3DCompile(source, sizeof source - 1, nullptr, nullptr, nullptr,
                               "vs_guest", "vs_5_0", 0, 0, &code, &errors)));
    uint32_t program[136][4] = {{0, 0, 0, 1}}, valid[136] = {15};
    float constants[192][4] = {};
    Nv2aGpuDraw state = {};
    state.vertex_program = program; state.vertex_valid = valid; state.vertex_constants = constants;
    for (uint32_t mask : {1u, 3u}) {
        VertexVariant variant;
        variant.key.assign(program[0], program[0] + 4);
        variant.input_mask = mask; variant.bytecode_bytes = code->GetBufferSize();
        assert(create_vertex_objects(code.Get(), variant.shader, variant.layout, mask));
        cache_shader_variant(vertex_variants, std::move(variant));
    }
    bool pending;
    for (uint32_t mask : {3u, 1u, 3u}) {
        state.attribute_mask = mask;
        auto *shader = get_vertex_shader(state, &pending);
        assert(shader && !pending && shader->input_mask == mask);
    }
    std::puts("PASS: real D3D11 layouts cached separately for the same vertex program");

    vertex_variants.clear();
    auto failed = std::make_shared<PixelCompileJob>();
    failed->vertex = true; failed->vertex_key.assign(program[0], program[0] + 4); failed->vertex_mask = 1;
    vertex_jobs_pending.emplace_back(failed->vertex_key, 1);
    vertex_jobs_pending.emplace_back(failed->vertex_key, 3);
    pixel_jobs_done.push_back(failed); pixel_jobs_finished = 1;
    install_compiled_pixel_shaders();
    assert(vertex_jobs_pending.size() == 1 && vertex_jobs_pending[0].second == 3);
    auto misses = gpu_timing.vertex_shader_misses;
    state.attribute_mask = 1;
    for (int i = 0; i < 100; i++) assert(!get_vertex_shader(state, &pending) && !pending);
    assert(gpu_timing.vertex_shader_misses == misses);

    /* An unsupported program is also suppressed on the synchronous path. */
    program[0][1] = 15u << 21;
    assert(!get_vertex_shader(state, &pending));
    misses = gpu_timing.vertex_shader_misses;
    for (int i = 0; i < 100; i++) assert(!get_vertex_shader(state, &pending));
    assert(gpu_timing.vertex_shader_misses == misses);

    const char pixel[] = "float4 ps_main():SV_Target{return 1;}";
    code.Reset(); errors.Reset();
    assert(SUCCEEDED(D3DCompile(pixel, sizeof pixel - 1, nullptr, nullptr, nullptr,
                               "ps_main", "ps_5_0", 0, 0, &code, &errors)));
    assert(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                               &generic_pixel_shader)));
    auto pixel_failed = std::make_shared<PixelCompileJob>();
    pixel_failed->key.fill(0);
    pixel_jobs_done.push_back(pixel_failed); pixel_jobs_finished = 1;
    install_compiled_pixel_shaders();
    misses = gpu_timing.pixel_shader_misses;
    for (int i = 0; i < 100; i++) assert(get_pixel_shader(state, 0) == generic_pixel_shader.Get());
    assert(gpu_timing.pixel_shader_misses == misses);
    std::puts("PASS: failed shaders are not retried; pending layouts stay distinct; pixel fallback remains available");
    return 0;
}
