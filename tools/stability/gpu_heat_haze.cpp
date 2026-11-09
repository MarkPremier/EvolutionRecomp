#include <cassert>
#include "../../../xboxrecomp_fork/src/kernel/nv2a_gpu_d3d11.cpp"

extern "C" {
void (*g_xbox_path_hook)(const char *) = nullptr;
int nv2a_pb_exec_batch_spans(float) { assert(false); return 0; }
void xbox_Nv2aAcknowledgeHandshakes(void) { assert(false); }
void xbox_CloakMarker(const char *) { assert(false); }
void xbox_FramebufferSize(uint32_t *w, uint32_t *h) { *w = 640; *h = 480; }
}

static void checked(HRESULT result, ID3DBlob *errors = nullptr)
{
    if (FAILED(result) && errors) std::fputs((const char *)errors->GetBufferPointer(), stderr);
    assert(SUCCEEDED(result));
}

static ComPtr<ID3D11Texture2D> texture(UINT w, UINT h, UINT flags, const uint32_t *pixels = nullptr)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = flags;
    D3D11_SUBRESOURCE_DATA data = {}; data.pSysMem = pixels; data.SysMemPitch = w * 4;
    ComPtr<ID3D11Texture2D> result;
    checked(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &result));
    return result;
}

int main()
{
    _putenv_s("RECOMP_SHADER_CACHE", "0");
    _putenv_s("RECOMP_SHADER_ASYNC", "0");
    D3D_FEATURE_LEVEL feature;
    checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                             D3D11_SDK_VERSION, &device, &feature, &context));

    uint8_t scene_copy = 0, target_memory = 0, static_texture = 0;
    Surface copy = {}; copy.memory = &scene_copy; copy.width = copy.height = 256; copy.pitch = 1024;
    surfaces.push_back(std::move(copy));
    Nv2aGpuDraw state = {};
    state.color = &target_memory; state.blend_enable = 1;
    state.blend_source = 0x302; state.blend_destination = 0x303;
    state.stage_program = 0x1801;
    state.textures[2].source = &scene_copy;
    state.textures[2].width = state.textures[2].height = 256; state.textures[2].format = 6;
    assert(heat_haze_stages(state) == 4);
    state.textures[2].source = &static_texture; assert(!heat_haze_stages(state));
    state.textures[2].source = &scene_copy;
    state.depth_mask = 1; assert(!heat_haze_stages(state)); state.depth_mask = 0;
    state.blend_source = 1; assert(!heat_haze_stages(state)); state.blend_source = 0x302;
    nv2a_gpu_set_heat_haze(3); assert(pending_heat_haze.load() == 3);
    surfaces.clear();
    std::puts("PASS: heat-haze detection excludes static bump materials and preserves the Off mode");

    uint32_t copy_program[136][4] = {{0, (1u << 21) | 0x1B, 2u << 26, 0xF800},
                                    {0, (1u << 21) | (9u << 9) | 0x1B, 2u << 26, 0xF849}};
    uint32_t copy_valid[136] = {15, 15}; float copy_constants[192][4] = {};
    Nv2aGpuVertex copy_vertices[3] = {};
    const float xy[3][2] = {{0, 0}, {256, 0}, {0, 256}};
    for (unsigned i = 0; i < 3; i++) {
        copy_vertices[i].attributes[0][0] = xy[i][0]; copy_vertices[i].attributes[0][1] = xy[i][1];
        copy_vertices[i].attributes[0][3] = 1;
        copy_vertices[i].attributes[9][0] = 320 + xy[i][0] * 2.5f;
        copy_vertices[i].attributes[9][1] = 120 + xy[i][1] * 0.9375f;
        copy_vertices[i].attributes[9][3] = 1;
    }
    Nv2aGpuDraw copy_state = {};
    copy_state.color = &target_memory; copy_state.width = copy_state.height = 256;
    copy_state.stage_program = 1; copy_state.vertex_program = copy_program;
    copy_state.vertex_valid = copy_valid; copy_state.vertex_constants = copy_constants;
    copy_state.textures[0].source = &scene_copy; copy_state.textures[0].linear = 1;
    copy_state.textures[0].format = 0x12; copy_state.textures[0].width = 1280; copy_state.textures[0].height = 480;
    heat_haze = 2;
    remember_heat_copy(copy_state, copy_vertices, 3, false);
    assert(heat_copies.count(&target_memory));
    const auto &mapping = heat_copies.at(&target_memory);
    assert(mapping.transform[0] == 0.5f && mapping.transform[1] == 0.5f);
    assert(mapping.transform[2] == 0.25f && mapping.transform[3] == 0.25f);
    heat_copies.clear();
    uint32_t colour_program[136][4] = {{0, (1u << 21) | 0x1B, 2u << 26, 0xF800},
                                      {0, (1u << 21) | (3u << 9) | 0x1B, 2u << 26, 0xF819}};
    Nv2aGpuDraw grid_state = {};
    grid_state.vertex_program = colour_program; grid_state.vertex_valid = copy_valid; grid_state.vertex_constants = copy_constants;
    grid_state.stage_program = 0x1801; grid_state.combiners.control = 0x11102;
    grid_state.combiners.final_input[0] = 0x0E; grid_state.combiners.final_input[1] = 0x1C80;
    grid_state.width = 1280; grid_state.height = 480; grid_state.aa_scale_x = 2;
    Nv2aGpuVertex grid_vertices[240] = {};
    for (auto &vertex : grid_vertices) {
        vertex.attributes[0][3] = 1; vertex.attributes[3][0] = 128.0f / 255.0f; vertex.attributes[3][3] = 1;
    }
    grid_vertices[239].attributes[0][0] = 320;
    assert(heat_grid_strength(grid_state, grid_vertices, 240) == 1);
    grid_vertices[239].attributes[0][0] = 480;
    assert(heat_grid_strength(grid_state, grid_vertices, 240) == 0.5f);
    grid_vertices[239].attributes[0][0] = 640;
    assert(heat_grid_strength(grid_state, grid_vertices, 240) == 0);
    grid_vertices[0].attributes[3][0] = 0;
    assert(heat_grid_strength(grid_state, grid_vertices, 240) == 1);
    assert(heat_grid_strength(grid_state, grid_vertices, 44) == 1);
    std::puts("PASS: oversized cube-grid fade excludes weapon haze");

    std::puts("PASS: screen-copy crop recovery preserves the original animated warp");

    const char vertex[] = R"HLSL(
Pixel heat_vertex(uint id:SV_VertexID) {
 float2 uv=float2((id<<1)&2,id&2);
 Pixel p=(Pixel)0;p.position=float4(uv*float2(2,-2)+float2(-1,1),0,1);
 p.diffuse=float4(factors[16].y,1,1,factors[17].a);p.tex0=float4(uv,0,1);p.tex2=float4(uv*(factors[17].z==0?1:factors[17].z)+factors[17].xy,0,1);p.depthW=1;p.depthZ=(factors[16].x>0?factors[16].x:0.5)*viewport.z;
 return p;
}
)HLSL";
    std::string source = std::string(nv2a_gpu_shader_source) + vertex;
    ComPtr<ID3DBlob> code, errors;
    HRESULT result = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                               "heat_vertex", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    checked(result, errors.Get());
    ComPtr<ID3D11VertexShader> vs;
    checked(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
    code.Reset(); errors.Reset();
    result = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                        "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    checked(result, errors.Get());
    ComPtr<ID3D11PixelShader> ps;
    checked(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
    context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> rs; checked(device->CreateRasterizerState(&raster, &rs));
    context->RSSetState(rs.Get());
    D3D11_BLEND_DESC blend = {};
    auto &b = blend.RenderTarget[0]; b.BlendEnable = TRUE;
    b.SrcBlend = D3D11_BLEND_SRC_ALPHA; b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D11_BLEND_ONE; b.DestBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> bs; checked(device->CreateBlendState(&blend, &bs));
    context->OMSetBlendState(bs.Get(), nullptr, UINT_MAX);
    D3D11_DEPTH_STENCIL_DESC depth = {}; depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> ds; checked(device->CreateDepthStencilState(&depth, &ds));
    context->OMSetDepthStencilState(ds.Get(), 0);
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampling; checked(device->CreateSamplerState(&sampler, &sampling));
    ID3D11SamplerState *samplers[4] = {sampling.Get(), sampling.Get(), sampling.Get(), sampling.Get()};
    context->PSSetSamplers(0, 4, samplers);
    uint32_t noise = 0xFF2020FF;
    auto noise_texture = texture(1, 1, D3D11_BIND_SHADER_RESOURCE, &noise);
    ComPtr<ID3D11ShaderResourceView> noise_view;
    checked(device->CreateShaderResourceView(noise_texture.Get(), nullptr, &noise_view));

    for (UINT height : {480u, 1440u, 2160u}) {
        context->PSSetShader(ps.Get(), nullptr, 0);
        if (height == 1440) {
            state.combiners.final_input[0] = 0x0A; state.combiners.final_input[1] = 0x1400;
            auto *specialized = get_pixel_shader(state, 0);
            assert(specialized && pixel_jobs_failed.empty());
            context->PSSetShader(specialized, nullptr, 0);
        }
        UINT width = height * 4 / 3;
        std::vector<uint32_t> pixels((size_t)width * height);
        for (UINT y = 0; y < height; y++) for (UINT x = 0; x < width; x++)
            pixels[(size_t)y * width + x] = 0xFF000000 | (x * 255 / width) | ((y * 255 / height) << 8);
        auto scene = texture(width, height, D3D11_BIND_SHADER_RESOURCE, pixels.data());
        auto output = texture(width, height, D3D11_BIND_RENDER_TARGET);
        context->CopyResource(output.Get(), scene.Get());
        ComPtr<ID3D11RenderTargetView> rtv;
        checked(device->CreateRenderTargetView(output.Get(), nullptr, &rtv));
        ID3D11RenderTargetView *rt = rtv.Get(); context->OMSetRenderTargets(1, &rt, nullptr);
        ComPtr<ID3D11ShaderResourceView> view;
        checked(device->CreateShaderResourceView(scene.Get(), nullptr, &view));
        ID3D11ShaderResourceView *views[4] = {noise_view.Get(), nullptr, view.Get(), nullptr};
        context->PSSetShaderResources(0, 4, views);
        Constants constants = {};
        constants.factors[17][3] = 1;
        constants.control[1] = 0x1801; constants.final_input[0] = 0x0A; constants.final_input[1] = 0x1400;
        constants.viewport[2] = 16777215; constants.depth_range[1] = 16777215;
        constants.texture_info[0][0] = constants.texture_info[0][1] = 128;
        constants.texture_info[2][0] = constants.texture_info[2][1] = 256;
        constants.bump_matrix[2][0] = constants.bump_matrix[2][3] = 0.4f;
        constants.bump_luminance[2][2] = 1.0f / width; constants.bump_luminance[2][3] = 1.0f / height;
        D3D11_BUFFER_DESC desc = {}; desc.ByteWidth = sizeof constants; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data = {}; data.pSysMem = &constants;
        ComPtr<ID3D11Buffer> buffer; checked(device->CreateBuffer(&desc, &data, &buffer));
        ID3D11Buffer *cb = buffer.Get(); context->PSSetConstantBuffers(0, 1, &cb); context->VSSetConstantBuffers(0, 1, &cb);
        D3D11_VIEWPORT viewport = {0, 0, (float)width, (float)height, 0, 1};
        context->RSSetViewports(1, &viewport); context->Draw(3, 0);
        D3D11_TEXTURE2D_DESC read_desc; output->GetDesc(&read_desc);
        read_desc.BindFlags = 0; read_desc.Usage = D3D11_USAGE_STAGING; read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback; checked(device->CreateTexture2D(&read_desc, nullptr, &readback));
        context->CopyResource(readback.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped; checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        auto at = [&](UINT x, UINT y) { return ((uint32_t *)((uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch))[x]; };
        /* The interior must still refract. At every outer edge the original
         * scene survives unchanged (alpha is deliberately allowed to fade). */
        UINT x = width / 2, y = height / 2;
        assert((at(x, y) & 0xFFFFFF) != (pixels[(size_t)y * width + x] & 0xFFFFFF));
        for (UINT edge = 0; edge < width; edge++) for (UINT row : {0u, height - 1})
            assert((at(edge, row) & 0xFFFFFF) == (pixels[(size_t)row * width + edge] & 0xFFFFFF));
        for (UINT row = 0; row < height; row++) for (UINT edge : {0u, width - 1})
            assert((at(edge, row) & 0xFFFFFF) == (pixels[(size_t)row * width + edge] & 0xFFFFFF));
        context->Unmap(readback.Get(), 0);
        /* +16 and -16 are adjacent across the signed-byte wrap. Their
         * midpoint is zero, not the huge offset made by unsigned filtering. */
        uint32_t signed_noise[2] = {0xFF1010FF, 0xFFF0F0FF};
        auto wrapped = texture(2, 1, D3D11_BIND_SHADER_RESOURCE, signed_noise);
        ComPtr<ID3D11ShaderResourceView> wrapped_view;
        checked(device->CreateShaderResourceView(wrapped.Get(), nullptr, &wrapped_view));
        views[0] = wrapped_view.Get(); context->PSSetShaderResources(0, 4, views);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(output.Get(), scene.Get());
        context->OMSetRenderTargets(1, &rt, nullptr); context->Draw(3, 0);
        context->CopyResource(readback.Get(), output.Get());
        checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        uint32_t actual = at(x, y), expected = pixels[(size_t)y * width + x];
        for (UINT shift : {0u, 8u})
            assert(std::abs((int)((actual >> shift) & 255) - (int)((expected >> shift) & 255)) <= 1);
        context->Unmap(readback.Get(), 0);
        ID3D11ShaderResourceView *empty[4] = {}; context->PSSetShaderResources(0, 4, empty);
        /* Weak barrel haze must retain the original blend alpha. */
        constants.factors[17][3] = 0.25f;
        context->UpdateSubresource(buffer.Get(), 0, nullptr, &constants, 0, 0);
        views[0] = noise_view.Get(); context->PSSetShaderResources(0, 4, views);
        context->OMSetRenderTargets(0, nullptr, nullptr); context->CopyResource(output.Get(), scene.Get());
        context->OMSetRenderTargets(1, &rt, nullptr); context->Draw(3, 0);
        context->CopyResource(readback.Get(), output.Get());
        checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        actual = at(x, y);
        assert((actual >> 24) == 64);  /* The combiner applies the faint particle alpha exactly once. */
        for (UINT shift : {0u, 8u})
            assert(std::abs((int)((actual >> shift) & 255) - (int)((expected >> shift) & 255) - 13) <= 2);
        context->Unmap(readback.Get(), 0);
        context->PSSetShaderResources(0, 4, empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        /* A cropped screen copy and an animated mesh coordinate: restore
         * the crop mapping so the warp survives the high-resolution upgrade.
         * An opacity of 0.25 previously got squared and hid barrel haze. */
        constants.factors[17][0] = -0.3f;
        constants.factors[17][1] = -0.5f;
        constants.factors[17][2] = 2;
        constants.texture_info[2][3] = 8192;
        constants.heat_transform[2][0] = constants.heat_transform[2][1] = 0.5f;
        constants.heat_transform[2][2] = constants.heat_transform[2][3] = 0.25f;
        std::memset(constants.bump_matrix[2], 0, sizeof constants.bump_matrix[2]);
        context->UpdateSubresource(buffer.Get(), 0, nullptr, &constants, 0, 0);
        context->CopyResource(output.Get(), scene.Get());
        context->PSSetShaderResources(0, 4, views);
        context->OMSetRenderTargets(1, &rt, nullptr); context->Draw(3, 0);
        context->CopyResource(readback.Get(), output.Get());
        checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        actual = at(x, y);
        assert(std::abs((int)(actual & 255) - (int)(expected & 255) - 6) <= 2);
        assert((actual >> 24) == 64);
        assert((at(0, y) & 0xFFFFFF) == (pixels[(size_t)y * width] & 0xFFFFFF));
        context->Unmap(readback.Get(), 0);
        context->PSSetShaderResources(0, 4, empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        std::printf("PASS: cropped scene mapping preserves mesh warp and faint barrel haze at %ux%u\n", width, height);
        /* The actual impact combiner also has to refract in Xbox mode,
         * which uses the original captured-scene UVs and original alpha. */
        Nv2aGpuDraw xbox_impact = {};
        xbox_impact.stage_program = 0x1801; xbox_impact.combiners.control = 0x11102;
        xbox_impact.combiners.final_input[0] = 0x0E; xbox_impact.combiners.final_input[1] = 0x1C80;
        xbox_impact.combiners.rgb_input[0] = 0xC4C90000; xbox_impact.combiners.alpha_input[0] = 0xD4D91010;
        xbox_impact.combiners.rgb_output[0] = xbox_impact.combiners.alpha_output[0] = 0xC0;
        xbox_impact.combiners.rgb_input[1] = 0xCA200000; xbox_impact.combiners.rgb_output[1] = 0xC0;
        auto *xbox_shader = get_pixel_shader(xbox_impact, 0); assert(xbox_shader);
        context->PSSetShader(xbox_shader, nullptr, 0);
        constants.control[0] = 2; constants.final_input[0] = 0x0E; constants.final_input[1] = 0x1C80;
        constants.final_input[2] = 0x11102;
        constants.stages[0][0] = 0xC4C90000; constants.stages[0][1] = 0xD4D91010;
        constants.stages[0][2] = constants.stages[0][3] = 0xC0;
        constants.stages[1][0] = 0xCA200000; constants.stages[1][2] = 0xC0;
        constants.factors[17][0] = constants.factors[17][1] = constants.factors[17][2] = 0;
        constants.texture_info[2][3] = 0;
        constants.bump_luminance[2][2] = constants.bump_luminance[2][3] = 0;
        constants.bump_matrix[2][0] = constants.bump_matrix[2][3] = 0.4f;
        context->UpdateSubresource(buffer.Get(), 0, nullptr, &constants, 0, 0);
        context->PSSetShaderResources(0, 4, views);
        context->CopyResource(output.Get(), scene.Get());
        context->OMSetRenderTargets(1, &rt, nullptr); context->Draw(3, 0);
        context->CopyResource(readback.Get(), output.Get());
        checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        actual = at(x, y);
        assert((actual >> 24) == 64);
        for (UINT shift : {0u, 8u})
            assert(std::abs((int)((actual >> shift) & 255) - (int)((expected >> shift) & 255) - 6) <= 2);
        context->Unmap(readback.Get(), 0);
        context->PSSetShaderResources(0, 4, empty); context->OMSetRenderTargets(0, nullptr, nullptr);
        std::printf("PASS: Xbox impact combiner retains refraction and original opacity at %ux%u\n", width, height);
        Surface destination = {}; destination.texture = output;
        destination.content_serial = ++target_content_serial;
        assert(get_heat_scene(destination));
        const float red[] = {1, 0, 0, 1}, green[] = {0, 1, 0, 1}, blue[] = {0, 0, 1, 1};
        context->ClearRenderTargetView(rt, red);
        destination.content_serial = ++target_content_serial; heat_scene.serial = destination.content_serial;  /* a haze-only write */
        assert(get_heat_scene(destination));
        context->CopyResource(readback.Get(), heat_scene.texture.Get());
        checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        assert(at(x, y) == actual);  /* consecutive haze keeps its original scene */
        context->Unmap(readback.Get(), 0);
        context->ClearRenderTargetView(rt, green); destination.content_serial = ++target_content_serial;  /* new scene content */
        assert(get_heat_scene(destination));
        context->CopyResource(readback.Get(), heat_scene.texture.Get());
        checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)); assert(at(x, y) == 0xFF00FF00);
        context->Unmap(readback.Get(), 0);
        context->ClearRenderTargetView(rt, blue); surface_frame_serial++;
        assert(get_heat_scene(destination));
        context->CopyResource(readback.Get(), heat_scene.texture.Get());
        checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)); assert(at(x, y) == 0xFFFF0000);
        context->Unmap(readback.Get(), 0);
        destination.width = destination.texture_width = width;
        destination.height = destination.texture_height = height;
        upload_surface(destination, nullptr, pixels.data(), width * 4);
        assert(get_heat_scene(destination));
        context->CopyResource(readback.Get(), heat_scene.texture.Get());
        checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)); assert(at(x, y) == expected);
        context->Unmap(readback.Get(), 0);
        std::printf("PASS: production heat shader has seamless borders and filters signed offsets correctly at %ux%u\n", width, height);
    }
    std::puts("PASS: heat batches share the scene copy; GPU/CPU scene writes and new frames refresh it");
    return 0;
}
