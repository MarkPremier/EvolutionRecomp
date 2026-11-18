#include <cassert>
#include "../../../xboxrecomp_fork/src/kernel/nv2a_gpu_d3d11.cpp"
extern "C" {
void (*g_xbox_path_hook)(const char *) = nullptr;
int nv2a_pb_exec_batch_spans(float) { assert(false);return 0; }
void xbox_Nv2aAcknowledgeHandshakes(void) { assert(false); }
void xbox_CloakMarker(const char *) { assert(false); }
void xbox_FramebufferSize(uint32_t *,uint32_t *) { assert(false); }
}
static ComPtr<ID3D11Texture2D> texture(UINT width,UINT height,UINT samples=1)
{
    D3D11_TEXTURE2D_DESC d={};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;
    d.SampleDesc.Count=samples;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> t;assert(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t)));return t;
}
int main()
{
    D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
                                     D3D11_SDK_VERSION,&device,&level,&context)));
    auto valid=texture(64,48),wide=texture(86,48),tall=texture(64,64),msaa=texture(64,48,2);
    ID3D11Resource *resources[4]={valid.Get(),valid.Get(),valid.Get(),valid.Get()};
    assert(dlss_inputs_match(resources[0],resources[1],resources[2],resources[3],64,48));
    for(int slot=0;slot<4;slot++) {
        for(ID3D11Resource *bad : {static_cast<ID3D11Resource *>(wide.Get()),static_cast<ID3D11Resource *>(tall.Get()),
                                  static_cast<ID3D11Resource *>(msaa.Get()),static_cast<ID3D11Resource *>(nullptr)}) {
            resources[slot]=bad;
            assert(!dlss_inputs_match(resources[0],resources[1],resources[2],resources[3],64,48));
        }
        resources[slot]=valid.Get();
    }
    D3D11_BUFFER_DESC d={};d.ByteWidth=64;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;assert(SUCCEEDED(device->CreateBuffer(&d,nullptr,&buffer)));
    assert(!dlss_inputs_match(buffer.Get(),valid.Get(),valid.Get(),valid.Get(),64,48));
    assert(dlss_inputs_match(wide.Get(),wide.Get(),wide.Get(),wide.Get(),86,48));
    assert(taa_pipeline());
    std::puts("PASS: DLAA accepts matching scene textures and rejects null, non-texture, mismatched and multisampled inputs before NGX");
}
