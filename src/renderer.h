#pragma once

#include "colorspace.h"
#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>

class Renderer {
public:
    Renderer() = default;

    ~Renderer();

    Renderer(const Renderer &) = delete;

    Renderer &operator=(const Renderer &) = delete;

    Renderer(Renderer &&) = delete;

    Renderer &operator=(Renderer &&) = delete;

    bool CreateDevice();

    void Shutdown();

    [[nodiscard]] ID3D11Device *Device() const;

    [[nodiscard]] ID3D11DeviceContext *Context() const;

    bool AddOutput(HWND hwnd, UINT width, UINT height);

    void ClearOutputs();

    [[nodiscard]] bool HasOutputs() const { return !outputs_.empty(); }

    void SetVideoSize(UINT width, UINT height);

    void SetSourceRect(float offsetU, float offsetV, float scaleU, float scaleV);

    void SetColorSpace(const ColorSpace& cs);

    bool RenderAll(ID3D11ShaderResourceView *srvY, ID3D11ShaderResourceView *srvUV) const;

private:
    struct Output {
        UINT width = 0;
        UINT height = 0;
        ComPtr<IDXGISwapChain1> swapChain;
        ComPtr<ID3D11RenderTargetView> rtv;
    };

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> vsConstants_;
    ComPtr<ID3D11Buffer> psConstants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;

    std::vector<Output> outputs_;

    bool CreateShaders();

    void UpdateColorMatrix();

    UINT videoWidth_ = 0;
    UINT videoHeight_ = 0;
    float sourceOffset_[2] = {0.0f, 0.0f};
    float sourceScale_[2] = {1.0f, 1.0f};
    ColorSpace colorSpace_;
    float colorMatrix_[12] = {};
};
