#include "VisualAnchor.h"
#include <windows.h>
#include <gdiplus.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

using namespace Gdiplus;

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL) return -1;
    
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

std::string ComputeSHA256Hash(const BYTE* pbData, DWORD cbData) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    DWORD cbHashObject = 0, cbHash = 0, cbDataOut = 0;
    PBYTE pbHashObject = NULL;
    PBYTE pbHash = NULL;
    std::string base64Hash = "";

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) 
        return base64Hash;

    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbDataOut, 0);
    pbHashObject = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHashObject);
    
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&cbHash, sizeof(DWORD), &cbDataOut, 0);
    pbHash = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHash);

    if (BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, pbHashObject, cbHashObject, NULL, 0, 0))) {
        if (BCRYPT_SUCCESS(BCryptHashData(hHash, (PBYTE)pbData, cbData, 0))) {
            if (BCRYPT_SUCCESS(BCryptFinishHash(hHash, pbHash, cbHash, 0))) {
                DWORD cchString = 0;
                CryptBinaryToStringA(pbHash, cbHash, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &cchString);
                std::vector<char> base64Buf(cchString);
                if (CryptBinaryToStringA(pbHash, cbHash, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64Buf.data(), &cchString)) {
                    base64Hash = std::string(base64Buf.data());
                }
            }
        }
    }

    if (hHash) BCryptDestroyHash(hHash);
    if (pbHashObject) HeapFree(GetProcessHeap(), 0, pbHashObject);
    if (pbHash) HeapFree(GetProcessHeap(), 0, pbHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);

    return base64Hash;
}

VisualAnchorArtifact CaptureLocalizedVisualAnchor(IUIAutomationElement* pElement) {
    VisualAnchorArtifact artifact;
    if (!pElement) return artifact;

    RECT bounds = {0};
    HRESULT hr = pElement->get_CurrentBoundingRectangle(&bounds);
    if (FAILED(hr)) return artifact;

    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    
    if (width <= 0 || height <= 0) return artifact;
    if (width > 200) width = 200;
    if (height > 200) height = 200;

    HDC hScreenDC = GetDC(NULL);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    HGDIOBJ hOldBitmap = SelectObject(hMemoryDC, hBitmap);

    BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, bounds.left, bounds.top, SRCCOPY);
    
    Gdiplus::Bitmap* pGdiBitmap = new Gdiplus::Bitmap(hBitmap, NULL);

    BitmapData bitmapData;
    Rect rect(0, 0, width, height);
    if (pGdiBitmap->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bitmapData) == Ok) {
        DWORD cbBitmapBytes = bitmapData.Stride * bitmapData.Height;
        
        // Audit Recommendation: Black-screen verification to ensure BitBlt didn't grab occluded geometry
        uint64_t totalLuma = 0;
        BYTE* pPixels = (BYTE*)bitmapData.Scan0;
        for (DWORD i = 0; i < cbBitmapBytes; i += 4) {
            // ARGB format: B=0, G=1, R=2, A=3
            BYTE b = pPixels[i];
            BYTE g = pPixels[i+1];
            BYTE r = pPixels[i+2];
            totalLuma += (r + g + b);
        }
        
        // If entirely black or nearly black, it's occluded/invalid. Return empty artifact.
        if (totalLuma < (width * height * 3)) { // Average < 1 per channel
             pGdiBitmap->UnlockBits(&bitmapData);
             delete pGdiBitmap;
             SelectObject(hMemoryDC, hOldBitmap);
             DeleteObject(hBitmap);
             DeleteDC(hMemoryDC);
             ReleaseDC(NULL, hScreenDC);
             return artifact;
        }

        artifact.base64Sha256Hash = ComputeSHA256Hash((BYTE*)bitmapData.Scan0, cbBitmapBytes);
        pGdiBitmap->UnlockBits(&bitmapData);
    }

    IStream* pStream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &pStream) == S_OK) {
        CLSID pngClsid;
        if (GetEncoderClsid(L"image/png", &pngClsid) != -1) {
            pGdiBitmap->Save(pStream, &pngClsid, NULL);

            HGLOBAL hGlobal = NULL;
            if (GetHGlobalFromStream(pStream, &hGlobal) == S_OK) {
                LPVOID pData = GlobalLock(hGlobal);
                DWORD dwSize = (DWORD)GlobalSize(hGlobal);

                DWORD cchString = 0;
                CryptBinaryToStringA((const BYTE*)pData, dwSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &cchString);
                std::vector<char> b64Buffer(cchString);
                CryptBinaryToStringA((const BYTE*)pData, dwSize, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64Buffer.data(), &cchString);
                
                artifact.base64Png = std::string(b64Buffer.data());
                GlobalUnlock(hGlobal);
            }
        }
        pStream->Release();
    }

    delete pGdiBitmap;
    SelectObject(hMemoryDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

    return artifact;
}
