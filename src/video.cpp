#include "video.h"

#include <mfapi.h>
#include <cstring>

void VideoPlayer::SourceRect(float *offsetU, float *offsetV, float *scaleU, float *scaleV) const {
    if (width_ == 0 || height_ == 0) {
        *offsetU = 0.0f;
        *offsetV = 0.0f;
        *scaleU = 1.0f;
        *scaleV = 1.0f;
        return;
    }

    const float texelU = 1.0f / static_cast<float>(width_);
    const float texelV = 1.0f / static_cast<float>(height_);
    const float u0 = static_cast<float>(visibleX_) * texelU + texelU * 0.5f;
    const float v0 = static_cast<float>(visibleY_) * texelV + texelV * 0.5f;
    const float u1 = static_cast<float>(visibleX_ + visibleWidth_) * texelU - texelU * 0.5f;
    const float v1 = static_cast<float>(visibleY_ + visibleHeight_) * texelV - texelV * 0.5f;

    *offsetU = u0;
    *offsetV = v0;
    *scaleU = u1 - u0;
    *scaleV = v1 - v0;
}

VideoPlayer::~VideoPlayer() {
    Close();
}

void VideoPlayer::Close() {
    if (reader_) {
        reader_->Flush(MF_SOURCE_READER_ALL_STREAMS);
        reader_.Reset();
    }
    srvY_.Reset();
    srvUV_.Reset();
    staging_.Reset();
    texture_.Reset();
    deviceManager_.Reset();
    device_ = nullptr;
    context_ = nullptr;
    width_ = 0;
    height_ = 0;
    visibleWidth_ = 0;
    visibleHeight_ = 0;
}

bool VideoPlayer::Open(const std::wstring &path, ID3D11Device *device, ID3D11DeviceContext *context) {
    device_ = device;
    context_ = context;

    CHECK_HR(MFCreateDXGIDeviceManager(&resetToken_, &deviceManager_));
    CHECK_HR(deviceManager_->ResetDevice(device_, resetToken_));

    ComPtr<IMFAttributes> attributes;
    CHECK_HR(MFCreateAttributes(&attributes, 4));
    CHECK_HR(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, deviceManager_.Get()));
    CHECK_HR(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    CHECK_HR(attributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE));

    CHECK_HR(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader_));

    if (!ConfigureStream())
        return false;
    return ReadMediaType();
}

bool VideoPlayer::ConfigureStream() const {
    // We need to ignore sound
    CHECK_HR(reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE));
    CHECK_HR(reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));

    ComPtr<IMFMediaType> wanted;
    CHECK_HR(MFCreateMediaType(&wanted));
    CHECK_HR(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));

    CHECK_HR(reader_->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, wanted.Get()));
    return true;
}

bool VideoPlayer::ReadMediaType() {
    ComPtr<IMFMediaType> current;
    CHECK_HR(reader_->GetCurrentMediaType( MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current));

    UINT32 width = 0;
    UINT32 height = 0;
    CHECK_HR(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height));
    if (width == 0 || height == 0)
        return false;

    UINT32 matrix = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_YUV_MATRIX, &matrix)))
        colorSpace_.bt601 = matrix == MFVideoTransferMatrix_BT601;
    else
        colorSpace_.bt601 = height < 720;

    UINT32 range = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)))
        colorSpace_.fullRange = (range == MFNominalRange_0_255);
    else
        colorSpace_.fullRange = false;

    if (width != width_ || height != height_) {
        if (!CreateTextures(width, height))
            return false;
    }
    ReadDisplayAperture(current.Get(), width, height);
    ++formatVersion_;
    return true;
}

void VideoPlayer::ReadDisplayAperture(IMFMediaType *type, const UINT width, const UINT height) {
    visibleX_ = 0;
    visibleY_ = 0;
    visibleWidth_ = width;
    visibleHeight_ = height;

    const GUID candidates[] = {
        MF_MT_MINIMUM_DISPLAY_APERTURE,
        MF_MT_PAN_SCAN_APERTURE,
        MF_MT_GEOMETRIC_APERTURE,
    };

    MFVideoArea area = {};
    for (const GUID &key: candidates) {
        if (SUCCEEDED(type->GetBlob(key, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
            const LONG areaWidth = area.Area.cx;
            const LONG areaHeight = area.Area.cy;
            if (areaWidth > 0 && areaHeight > 0 &&
                area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
                static_cast<UINT>(area.OffsetX.value + areaWidth) <= width &&
                static_cast<UINT>(area.OffsetY.value + areaHeight) <= height) {
                visibleX_ = static_cast<UINT>(area.OffsetX.value);
                visibleY_ = static_cast<UINT>(area.OffsetY.value);
                visibleWidth_ = static_cast<UINT>(areaWidth);
                visibleHeight_ = static_cast<UINT>(areaHeight);
                break;
            }
        }
    }
}

bool VideoPlayer::CreateTextures(const UINT width, const UINT height) {
    srvY_.Reset();
    srvUV_.Reset();
    staging_.Reset();
    texture_.Reset();

    // Needed by NV12
    const UINT alignedWidth = (width + 1) & ~1u;
    const UINT alignedHeight = (height + 1) & ~1u;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = alignedWidth;
    desc.Height = alignedHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    CHECK_HR(device_->CreateTexture2D(&desc, nullptr, &texture_));

    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    srv.Format = DXGI_FORMAT_R8_UNORM;
    CHECK_HR(device_->CreateShaderResourceView(texture_.Get(), &srv, &srvY_));

    srv.Format = DXGI_FORMAT_R8G8_UNORM;
    CHECK_HR(device_->CreateShaderResourceView(texture_.Get(), &srv, &srvUV_));

    width_ = alignedWidth;
    height_ = alignedHeight;
    visibleX_ = 0;
    visibleY_ = 0;
    visibleWidth_ = alignedWidth;
    visibleHeight_ = alignedHeight;
    return true;
}

bool VideoPlayer::Restart() const {
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = 0;
    const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    return SUCCEEDED(hr);
}

bool VideoPlayer::DecodeNext(LONGLONG *timestamp, bool *looped) {
    *looped = false;

    for (int attempt = 0; attempt < 16; ++attempt) {
        DWORD streamFlags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;

        const HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               0, nullptr, &streamFlags, &ts, &sample);
        if (FAILED(hr))
            return false;

        if (streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            if (!ReadMediaType())
                return false;
        }

        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (!Restart())
                return false;
            *looped = true;
            continue;
        }

        if (!sample)
            continue;

        if (!UploadSample(sample.Get()))
            return false;

        *timestamp = ts;
        return true;
    }

    return false;
}

bool VideoPlayer::UploadSample(IMFSample *sample) {
    DWORD bufferCount = 0;
    sample->GetBufferCount(&bufferCount);

    ComPtr<IMFMediaBuffer> buffer;
    const HRESULT hr = (bufferCount == 1)
                           ? sample->GetBufferByIndex(0, &buffer)
                           : sample->ConvertToContiguousBuffer(&buffer);
    CHECK_HR(hr);

    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (SUCCEEDED(buffer.As(&dxgiBuffer))) {
        ComPtr<ID3D11Texture2D> source;
        if (SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(&source)))) {
            UINT subresource = 0;
            dxgiBuffer->GetSubresourceIndex(&subresource);
            context_->CopySubresourceRegion(texture_.Get(), 0, 0, 0, 0,
                                            source.Get(), subresource, nullptr);
            return true;
        }
    }

    return UploadFromSystemMemory(buffer.Get());
}

bool VideoPlayer::UploadFromSystemMemory(IMFMediaBuffer *buffer) {
    if (!staging_) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width_;
        desc.Height = height_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CHECK_HR(device_->CreateTexture2D(&desc, nullptr, &staging_));
    }

    BYTE *source = nullptr;
    LONG sourcePitch = 0;
    bool locked2D = false;

    ComPtr<IMF2DBuffer> buffer2D;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2D))) &&
        SUCCEEDED(buffer2D->Lock2D(&source, &sourcePitch))) {
        locked2D = true;
    } else {
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        CHECK_HR(buffer->Lock(&source, &maxLength, &currentLength));
        sourcePitch = static_cast<LONG>(width_);
    }

    if (sourcePitch <= 0 || static_cast<UINT>(sourcePitch) < width_) {
        if (locked2D)
            buffer2D->Unlock2D();
        else
            buffer->Unlock();
        return false;
    }

    bool ok = false;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context_->Map(staging_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) {
        auto *destination = static_cast<BYTE *>(mapped.pData);
        const size_t rowBytes = width_;

        for (UINT y = 0; y < height_; ++y)
            memcpy(destination + static_cast<size_t>(y) * mapped.RowPitch,
                   source + static_cast<size_t>(y) * sourcePitch, rowBytes);

        BYTE *destinationUV = destination + static_cast<size_t>(mapped.RowPitch) * height_;
        const BYTE *sourceUV = source + static_cast<size_t>(sourcePitch) * height_;
        for (UINT y = 0; y < height_ / 2; ++y)
            memcpy(destinationUV + static_cast<size_t>(y) * mapped.RowPitch,
                   sourceUV + static_cast<size_t>(y) * sourcePitch, rowBytes);

        context_->Unmap(staging_.Get(), 0);
        context_->CopyResource(texture_.Get(), staging_.Get());
        ok = true;
    }

    if (locked2D)
        buffer2D->Unlock2D();
    else
        buffer->Unlock();

    return ok;
}