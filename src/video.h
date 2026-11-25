#pragma once

#include "colorspace.h"
#include "common.h"

#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <string>

class VideoPlayer {
public:
    VideoPlayer() = default;
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;
    VideoPlayer(VideoPlayer&&) = delete;
    VideoPlayer& operator=(VideoPlayer&&) = delete;

    bool Open(const std::wstring& path, ID3D11Device* device, ID3D11DeviceContext* context);
    void Close();

    bool DecodeNext(LONGLONG* timestamp, bool* looped);

    [[nodiscard]] bool Restart() const;

    [[nodiscard]] ID3D11ShaderResourceView* PlaneY() const { return srvY_.Get(); }
    [[nodiscard]] ID3D11ShaderResourceView* PlaneUV() const { return srvUV_.Get(); }

    void SourceRect(float* offsetU, float* offsetV, float* scaleU, float* scaleV) const;

    [[nodiscard]] UINT Width() const { return visibleWidth_; }
    [[nodiscard]] UINT Height() const { return visibleHeight_; }
    [[nodiscard]] const ColorSpace& GetColorSpace() const { return colorSpace_; }

    [[nodiscard]] unsigned FormatVersion() const { return formatVersion_; }

private:

    [[nodiscard]] bool ConfigureStream() const;
    bool ReadMediaType();
    bool CreateTextures(UINT width, UINT height);
    void ReadDisplayAperture(IMFMediaType* type, UINT width, UINT height);
    bool UploadSample(IMFSample* sample);
    bool UploadFromSystemMemory(IMFMediaBuffer* buffer);

    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11Texture2D> staging_; // Only used when fallback software mode
    ComPtr<ID3D11ShaderResourceView> srvY_;
    ComPtr<ID3D11ShaderResourceView> srvUV_;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    UINT resetToken_ = 0;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT visibleX_ = 0;
    UINT visibleY_ = 0;
    UINT visibleWidth_ = 0;
    UINT visibleHeight_ = 0;
    unsigned formatVersion_ = 0;
    ColorSpace colorSpace_;
};
