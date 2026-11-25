#include "renderer.h"

#include <d3d10.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace {
    constexpr float kBt709Limited[12] = {
        1.164383f, 0.000000f, 1.792741f, -0.969433f,
        1.164383f, -0.213249f, -0.532909f, 0.300016f,
        1.164383f, 2.112402f, 0.000000f, -1.129264f,
    };
    constexpr float kBt601Limited[12] = {
        1.164383f, 0.000000f, 1.596027f, -0.870076f,
        1.164383f, -0.391762f, -0.812968f, 0.529302f,
        1.164383f, 2.017232f, 0.000000f, -1.081679f,
    };
    constexpr float kBt709Full[12] = {
        1.000000f, 0.000000f, 1.574800f, -0.787400f,
        1.000000f, -0.187324f, -0.468124f, 0.327724f,
        1.000000f, 1.855600f, 0.000000f, -0.927800f,
    };
    constexpr float kBt601Full[12] = {
        1.000000f, 0.000000f, 1.402000f, -0.701000f,
        1.000000f, -0.344136f, -0.714136f, 0.529136f,
        1.000000f, 1.772000f, 0.000000f, -0.886000f,
    };

    struct VSConstants {
        float scaleX;
        float scaleY;
        float offsetU;
        float offsetV;
        float scaleU;
        float scaleV;
        float padding[2];
    };

    struct PSConstants {
        float matrix[12];
    };

    bool CompileShader(const char *source, const size_t length, const char *target, ComPtr<ID3DBlob> &blob) {
        return SUCCEEDED(D3DCompile(source, length, nullptr, nullptr, nullptr, "main", target,
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, nullptr));
    }

    constexpr char kVertexShader[] = R"HLSL(
cbuffer VSConstants : register(b0)
{
    float2 scale;      // taille du quad en NDC (cadrage cover)
    float2 uvOffset;   // coin haut-gauche de la zone utile de la texture
    float2 uvScale;    // etendue de cette zone
    float2 padding;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    float2 corner = float2((vertexId & 1) ? 1.0 : -1.0,
                           (vertexId & 2) ? -1.0 : 1.0);
    VSOutput output;
    output.position = float4(corner * scale, 0.0, 1.0);
    float2 corner01 = float2((vertexId & 1) ? 1.0 : 0.0,
                             (vertexId & 2) ? 1.0 : 0.0);
    output.uv = corner01 * uvScale + uvOffset;
    return output;
}
)HLSL";

    constexpr char kPixelShader[] = R"HLSL(
Texture2D<float>  planeY  : register(t0);
Texture2D<float2> planeUV : register(t1);
SamplerState      linearSampler : register(s0);

cbuffer PSConstants : register(b0)
{
    float4 matrixR;  // xyz = coefficients YUV, w = offset
    float4 matrixG;
    float4 matrixB;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float3 yuv;
    yuv.x = planeY.Sample(linearSampler, input.uv);
    yuv.yz = planeUV.Sample(linearSampler, input.uv);

    float3 rgb;
    rgb.r = dot(matrixR.xyz, yuv) + matrixR.w;
    rgb.g = dot(matrixG.xyz, yuv) + matrixG.w;
    rgb.b = dot(matrixB.xyz, yuv) + matrixB.w;

    return float4(saturate(rgb), 1.0);
}
)HLSL";
}

Renderer::~Renderer() {
    Shutdown();
}

bool Renderer::CreateDevice() {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    CHECK_HR(
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, std::size(levels),
            D3D11_SDK_VERSION, &device_, nullptr, &context_));
    UINT nv12Support = 0;
    if (FAILED(device_->CheckFormatSupport(DXGI_FORMAT_NV12, &nv12Support)) || (nv12Support &
            D3D11_FORMAT_SUPPORT_TEXTURE2D) == 0) {
        return false;
    }

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice1> dxgiDevice;
    CHECK_HR(device_.As(&dxgiDevice));
    dxgiDevice->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    CHECK_HR(dxgiDevice->GetAdapter(&adapter));
    CHECK_HR(adapter->GetParent(IID_PPV_ARGS(&factory_)));

    if (!CreateShaders())
        return false;

    UpdateColorMatrix();
    return true;
}

void Renderer::Shutdown() {
    ClearOutputs();
    raster_.Reset();
    sampler_.Reset();
    psConstants_.Reset();
    vsConstants_.Reset();
    ps_.Reset();
    vs_.Reset();
    factory_.Reset();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    context_.Reset();
    device_.Reset();
}

ID3D11Device *Renderer::Device() const { return device_.Get(); }

ID3D11DeviceContext *Renderer::Context() const { return context_.Get(); }

bool Renderer::AddOutput(const HWND hwnd, const UINT width, const UINT height)
{
    if (!device_ || !factory_ || width == 0 || height == 0)
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    Output output;
    output.width = width;
    output.height = height;

    CHECK_HR(factory_->CreateSwapChainForHwnd(device_.Get(), hwnd, &desc, nullptr, nullptr,
                                              &output.swapChain));
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    ComPtr<ID3D11Texture2D> backBuffer;
    CHECK_HR(output.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    CHECK_HR(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &output.rtv));

    outputs_.push_back(std::move(output));
    return true;
}

void Renderer::ClearOutputs() {
    if (context_)
        context_->ClearState();
    outputs_.clear();
    if (context_)
        context_->Flush();
}

bool Renderer::CreateShaders() {
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(kVertexShader, sizeof(kVertexShader) - 1, "vs_4_0", vsBlob))
        return false;
    if (!CompileShader(kPixelShader, sizeof(kPixelShader) - 1, "ps_4_0", psBlob))
        return false;

    CHECK_HR(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_));
    CHECK_HR(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_));

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    bd.ByteWidth = sizeof(VSConstants);
    CHECK_HR(device_->CreateBuffer(&bd, nullptr, &vsConstants_));

    bd.ByteWidth = sizeof(PSConstants);
    CHECK_HR(device_->CreateBuffer(&bd, nullptr, &psConstants_));

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK_HR(device_->CreateSamplerState(&sd, &sampler_));

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    CHECK_HR(device_->CreateRasterizerState(&rd, &raster_));

    return true;
}

void Renderer::UpdateColorMatrix() {
    static const float *const kMatrices[2][2] = {
        {kBt709Limited, kBt601Limited},
        {kBt709Full, kBt601Full},
    };
    const float *const source = kMatrices[colorSpace_.fullRange][colorSpace_.bt601];
    memcpy(colorMatrix_, source, sizeof(colorMatrix_));
}

void Renderer::SetVideoSize(const UINT width, const UINT height)
{
    videoWidth_ = width;
    videoHeight_ = height;
}


void Renderer::SetSourceRect(const float offsetU, const float offsetV, const float scaleU, const float scaleV)
{
    sourceOffset_[0] = offsetU;
    sourceOffset_[1] = offsetV;
    sourceScale_[0] = scaleU;
    sourceScale_[1] = scaleV;
}

void Renderer::SetColorSpace(const ColorSpace& cs)
{
    colorSpace_ = cs;
    UpdateColorMatrix();
}

bool Renderer::RenderAll(ID3D11ShaderResourceView *srvY, ID3D11ShaderResourceView *srvUV) const {
    if (!context_ || outputs_.empty() || srvY == nullptr || srvUV == nullptr)
        return true;

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context_->Map(psConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return true;
        auto *constants = static_cast<PSConstants *>(mapped.pData);
        memcpy(constants->matrix, colorMatrix_, sizeof(colorMatrix_));
        context_->Unmap(psConstants_.Get(), 0);
    }

    ID3D11ShaderResourceView *const srvs[2] = {srvY, srvUV};
    ID3D11SamplerState *const samplers[1] = {sampler_.Get()};
    // NOLINTNEXTLINE(misc-const-correctness)
    ID3D11Buffer *const vsCb = vsConstants_.Get();
    // NOLINTNEXTLINE(misc-const-correctness)
    ID3D11Buffer *const psCb = psConstants_.Get();

    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->RSSetState(raster_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &vsCb);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &psCb);
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 1, samplers);

    bool deviceLost = false;

    for (const Output &output: outputs_) {
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        if (videoWidth_ > 0 && videoHeight_ > 0) {
            const double sx = static_cast<double>(output.width) / videoWidth_;
            const double sy = static_cast<double>(output.height) / videoHeight_;
            const double s = (std::max)(sx, sy);
            scaleX = static_cast<float>(videoWidth_ * s / output.width);
            scaleY = static_cast<float>(videoHeight_ * s / output.height);
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context_->Map(vsConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            continue;
        auto *constants = static_cast<VSConstants *>(mapped.pData);
        constants->scaleX = scaleX;
        constants->scaleY = scaleY;
        constants->offsetU = sourceOffset_[0];
        constants->offsetV = sourceOffset_[1];
        constants->scaleU = sourceScale_[0];
        constants->scaleV = sourceScale_[1];
        constants->padding[0] = constants->padding[1] = 0.0f;
        context_->Unmap(vsConstants_.Get(), 0);

        // NOLINTNEXTLINE(misc-const-correctness)
        ID3D11RenderTargetView *rtv = output.rtv.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);

        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(output.width);
        viewport.Height = static_cast<float>(output.height);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);

        context_->Draw(4, 0);

        if (const HRESULT hr = output.swapChain->Present(0, 0);
            hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            deviceLost = true;
    }

    constexpr ID3D11ShaderResourceView *const nullSrvs[2] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, nullSrvs);

    return !deviceLost;
}