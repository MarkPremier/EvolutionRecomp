#include <cassert>
#include "../../../xboxrecomp_fork/src/kernel/nv2a_gpu_d3d11.cpp"

extern "C" {
void (*g_xbox_path_hook)(const char *) = nullptr;
int nv2a_pb_exec_batch_spans(float) { assert(false); return 0; }
void xbox_Nv2aAcknowledgeHandshakes(void) { assert(false); }
void xbox_CloakMarker(const char *) { assert(false); }
void xbox_FramebufferSize(uint32_t *, uint32_t *) { assert(false); }
}

static ComPtr<ID3DBlob> compile(const std::string &source, const char *entry, const char *profile)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT result = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                                entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                                0, &code, &errors);
    if (FAILED(result) && errors) std::fputs((const char *)errors->GetBufferPointer(), stderr);
    assert(SUCCEEDED(result));
    return code;
}

int main()
{
    static constexpr float depth_range = 65535.0f;
    static constexpr float depth_a = depth_range / (1.0f - 0.1f / 690.0f);
    static constexpr float depth_b = 0.1f * depth_a;
    const auto projected = [](float q) { return depth_a - depth_b / q; };

    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &device, nullptr, &context)));
    auto vert = compile(std::string(nv2a_gpu_shader_source) + R"HLSL(
Pixel test_vertex(uint id:SV_VertexID) {
 Pixel p=(Pixel)0;
 float2 corner=float2((id<<1)&2,id&2);
 p.position=float4(corner*float2(2,-2)+float2(-1,1),0,1);
 p.depthZ=p.depthClip=0.5*viewport.z;
 p.depthW=1;
 p.tex3=factors[0];
 return p;
}
)HLSL", "test_vertex", "vs_5_0");
    auto pixel = compile(nv2a_gpu_shader_source, "ps_main", "ps_5_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    assert(SUCCEEDED(device->CreateVertexShader(vert->GetBufferPointer(), vert->GetBufferSize(), nullptr, &vs)));
    assert(SUCCEEDED(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps)));
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp = {0, 0, 1, 1, 0, 1};
    context->RSSetViewports(1, &vp);
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> rs;
    assert(SUCCEEDED(device->CreateRasterizerState(&raster, &rs)));
    context->RSSetState(rs.Get());

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampling;
    assert(SUCCEEDED(device->CreateSamplerState(&sampler, &sampling)));
    context->PSSetSamplers(3, 1, sampling.GetAddressOf());
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> map, output, staging;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &map)));
    ComPtr<ID3D11ShaderResourceView> view;
    assert(SUCCEEDED(device->CreateShaderResourceView(map.Get(), nullptr, &view)));
    context->PSSetShaderResources(3, 1, view.GetAddressOf());
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &output)));
    ComPtr<ID3D11RenderTargetView> target;
    assert(SUCCEEDED(device->CreateRenderTargetView(output.Get(), nullptr, &target)));
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));

    D3D11_BUFFER_DESC constants_desc = {};
    constants_desc.ByteWidth = sizeof(Constants);
    constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    assert(SUCCEEDED(device->CreateBuffer(&constants_desc, nullptr, &buffer)));
    context->VSSetConstantBuffers(0, 1, buffer.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, buffer.GetAddressOf());

    const auto sample = [&](float receiver, float caster, float coverage = 1.0f) {
        const float depth = projected(caster) / depth_range;
        context->UpdateSubresource(map.Get(), 0, nullptr, &depth, sizeof depth, 0);
        Constants c = {};
        c.control[1] = 2 << 15;   /* 3D-projective texture in stage 3 */
        c.final_input[0] = 11;   /* display the shadow comparison directly */
        c.viewport[2] = c.depth_range[1] = depth_range;
        /* The Chapter 12 capture's light basis and a texel on the guard.
         * The shadow map stand-in uses the texel as its center. */
        const float s[] = {-2368.25f, 614.702f, 4102.61f};
        const float t[] = {4021.14f, 763.082f, 2462.72f};
        const float w[] = {0.138699f, -0.87644f, 0.461103f, 137.503f};
        std::memcpy(c.vertex_constants[144], s, sizeof s);
        std::memcpy(c.vertex_constants[145], t, sizeof t);
        std::memcpy(c.vertex_constants[147], w, sizeof w);
        for (int i = 0; i < 4; i++) c.vertex_constants[146][i] = depth_a * w[i];
        c.vertex_constants[146][3] -= depth_b;
        const float q = receiver * coverage;
        c.factors[0][0] = 580.0f * q;
        c.factors[0][1] = 1021.0f * q;
        c.factors[0][2] = projected(receiver) * q;
        c.factors[0][3] = q;
        c.texture_info[3][0] = 1160.0f;
        c.texture_info[3][1] = 2042.0f;
        c.texture_info[3][2] = 1;
        c.texture_info[3][3] = (float)(64u | (4u << 8) | 2048u | 4096u |
            (balcony_shadow_fade(true, 3, 0x30) ? 32768u : 0u));
        context->UpdateSubresource(buffer.Get(), 0, nullptr, &c, 0, 0);
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE result = {};
        assert(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &result)));
        const float value = *static_cast<const float *>(result.pData);
        context->Unmap(staging.Get(), 0);
        return value;
    };

    nv2a_gpu_set_shadow_indoor(1);
    for (int mode : {0, 1}) {
        nv2a_gpu_set_shadow_mode(mode);
        assert(!balcony_shadow_fade(true, 3, 0x30));
        assert(sample(283.0f, 275.1f) < 0.01f);
    }
    nv2a_gpu_set_shadow_mode(2);
    assert(!balcony_shadow_fade(false, 3, 0x30));
    assert(!balcony_shadow_fade(true, 2, 0x30));
    assert(!balcony_shadow_fade(true, 3, 0x31));
    assert(sample(278.4f, 275.1f) < 0.01f);  /* upper body on the same floor */
    assert(sample(278.4f, 277.5f) < 0.01f);  /* legs on that floor */
    const float transition = sample(278.8f, 275.1f);
    assert(transition > 0.30f && transition < 0.65f);
    assert(sample(283.0f, 275.1f) > 0.99f);
    assert(sample(283.0f, 277.5f) > 0.99f);
    assert(sample(283.0f, 277.5f, 0.15f) > 0.99f);
    nv2a_gpu_set_shadow_indoor(0);
    assert(sample(283.0f, 275.1f) < 0.01f);
    std::puts("PASS: Enhanced+ keeps same-floor shadows solid and fades spill onto the floor below");
}
