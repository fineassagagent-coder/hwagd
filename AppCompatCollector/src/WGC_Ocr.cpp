#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include "hwagd_structs.h"
#include <iostream>

// Compile-time feature flag - defaults to OFF until border suppression verified
#ifndef HWAGD_ENABLE_WGC
#define HWAGD_ENABLE_WGC 0
#endif

#if HWAGD_ENABLE_WGC

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <dxgi.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Graphics::DirectX;

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice CreateDirect3DDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    winrt::com_ptr<ID3D11Device> d3dDevice;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                      nullptr, 0, D3D11_SDK_VERSION, d3dDevice.put(), nullptr, nullptr);
    auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
    return inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

std::wstring ExtractTextFromHWND(HWND targetHwnd) {
    init_apartment(apartment_type::multi_threaded);
    auto activation_factory = get_activation_factory<GraphicsCaptureItem>();
    auto interop_factory = activation_factory.as<IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interop_factory->CreateForWindow(targetHwnd,
        guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), put_abi(item));
    if (FAILED(hr) || !item) return L"";

    auto framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        CreateDirect3DDevice(), DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
    auto session = framePool.CreateCaptureSession(item);

    auto session3 = session.try_as<IGraphicsCaptureSession3>();
    if (session3) { session3.IsBorderRequired(false); }

    session.StartCapture();
    auto frame = framePool.TryGetNextFrame();
    if (!frame) { session.Close(); framePool.Close(); return L""; }

    auto softwareBitmap = SoftwareBitmap::CreateCopyFromSurfaceAsync(frame.Surface()).get();
    auto ocrEngine = OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(L"en-US"));
    std::wstring extractedText;
    if (ocrEngine) {
        auto result = ocrEngine.RecognizeAsync(softwareBitmap).get();
        extractedText = std::wstring(result.Text().c_str());
    }
    session.Close(); framePool.Close();
    return extractedText;
}

#else

std::wstring ExtractTextFromHWND(HWND targetHwnd) {
    (void)targetHwnd;
    return L"";  // WGC disabled until border suppression validated
}

#endif
