#include <cassert>
#include "../../../xboxrecomp_fork/src/kernel/nv2a_gpu_d3d11.cpp"
extern "C" {
void (*g_xbox_path_hook)(const char *) = nullptr;
int nv2a_pb_exec_batch_spans(float) { assert(false);return 0; }
void xbox_Nv2aAcknowledgeHandshakes(void) { assert(false); }
void xbox_CloakMarker(const char *) { assert(false); }
void xbox_FramebufferSize(uint32_t *,uint32_t *) { assert(false); }
}
static ComPtr<ID3D11Texture2D> texture(UINT width, UINT height, DXGI_FORMAT format, UINT flags, UINT samples = 1)
{
    D3D11_TEXTURE2D_DESC d={};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;
    d.SampleDesc.Count=samples;d.Format=format;d.BindFlags=flags;
    ComPtr<ID3D11Texture2D> t;assert(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t)));return t;
}
int main()
{
    D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
                                     D3D11_SDK_VERSION,&device,&level,&context)));
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R32_FLOAT,
                                   DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    ComPtr<ID3D11Texture2D> valid[4], resized[4];
    ID3D11Resource *resources[4];
    for (unsigned i = 0; i < 4; i++) {
        UINT flags = i == 3 ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE;
        valid[i] = texture(64, 48, formats[i], flags);
        resized[i] = texture(86, 48, formats[i], flags);
        resources[i] = valid[i].Get();
    }
    assert(dlss_inputs_match(resources[0],resources[1],resources[2],resources[3],64,48));
    for(int slot=0;slot<4;slot++) {
        auto no_bind = texture(64, 48, formats[slot], D3D11_BIND_RENDER_TARGET);
        auto tall = texture(64, 64, formats[slot], D3D11_BIND_SHADER_RESOURCE);
        auto msaa = texture(64, 48, formats[slot], D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 2);
        for(ID3D11Resource *bad : {static_cast<ID3D11Resource *>(resized[slot].Get()),
                                  static_cast<ID3D11Resource *>(tall.Get()), static_cast<ID3D11Resource *>(msaa.Get()),
                                  static_cast<ID3D11Resource *>(no_bind.Get()),
                                  static_cast<ID3D11Resource *>(valid[(slot + 1) % 4].Get()),
                                  static_cast<ID3D11Resource *>(nullptr)}) {
            resources[slot]=bad;
            assert(!dlss_inputs_match(resources[0],resources[1],resources[2],resources[3],64,48));
        }
        resources[slot]=valid[slot].Get();
    }
    D3D11_BUFFER_DESC d={};d.ByteWidth=64;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;assert(SUCCEEDED(device->CreateBuffer(&d,nullptr,&buffer)));
    assert(!dlss_inputs_match(buffer.Get(),valid[1].Get(),valid[2].Get(),valid[3].Get(),64,48));
    assert(dlss_inputs_match(resized[0].Get(),resized[1].Get(),resized[2].Get(),resized[3].Get(),86,48));
    assert(taa_pipeline());
    std::puts("PASS: DLAA rejects incompatible dimensions, sample counts, formats and resource bindings before NGX");
}
