#include "hwagd_structs.h"
#include "PIIRedactor.h"
#include <uiautomation.h>
#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include "VisualAnchor.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

IUIAutomation* g_pAutomation = nullptr;
DWORD g_staThreadId = 0;
std::queue<HWND> g_hwndQueue;
std::mutex g_queueMutex;

// Forward declaration
void ProcessUIAEvent(HWND hwnd);

extern void OnApplicationStateChanged(const ApplicationStateNode& node);

std::wstring BstrToWString(BSTR bstr) {
    if (!bstr) return L"";
    std::wstring result(bstr);
    SysFreeString(bstr);
    return result;
}

UIAContextMatrix ExtractElementProperties(IUIAutomationElement* pElement) {
    UIAContextMatrix matrix;
    if (!pElement) return matrix;

    BSTR bstrId = nullptr;
    if (SUCCEEDED(pElement->get_CurrentAutomationId(&bstrId))) {
        matrix.automationId = BstrToWString(bstrId);
    }

    IUIAutomationValuePattern* pValuePattern = nullptr;
    if (SUCCEEDED(pElement->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pValuePattern))) && pValuePattern) {
        BSTR bstrValue = nullptr;
        if (SUCCEEDED(pValuePattern->get_CurrentValue(&bstrValue))) {
            matrix.valueText = BstrToWString(bstrValue);
        }
        pValuePattern->Release();
    }

    IUIAutomationTextPattern* pTextPattern = nullptr;
    if (SUCCEEDED(pElement->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pTextPattern))) && pTextPattern) {
        IUIAutomationTextRange* pTextRange = nullptr;
        if (SUCCEEDED(pTextPattern->get_DocumentRange(&pTextRange)) && pTextRange) {
            BSTR bstrText = nullptr;
            if (SUCCEEDED(pTextRange->GetText(-1, &bstrText))) {
                matrix.rangeText = BstrToWString(bstrText);
            }
            pTextRange->Release();
        }
        pTextPattern->Release();
    }

    IUIAutomationLegacyIAccessiblePattern* pLegacyPattern = nullptr;
    if (SUCCEEDED(pElement->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&pLegacyPattern))) && pLegacyPattern) {
        BSTR bstrLegacy = nullptr;
        if (SUCCEEDED(pLegacyPattern->get_CurrentValue(&bstrLegacy))) {
            matrix.legacyValue = BstrToWString(bstrLegacy);
        }
        pLegacyPattern->Release();
    }

    return matrix;
}

std::vector<UIAContextMatrix> BuildSiblingContextMatrix(IUIAutomation* pAutomation, IUIAutomationElement* pFocusedElement, int maxSiblings) {
    std::vector<UIAContextMatrix> contextList;
    if (!pAutomation || !pFocusedElement) return contextList;

    IUIAutomationTreeWalker* pTreeWalker = nullptr;
    if (FAILED(pAutomation->get_ControlViewWalker(&pTreeWalker)) || !pTreeWalker) return contextList;

    IUIAutomationElement* pCurrentNode = nullptr;
    pTreeWalker->GetNextSiblingElement(pFocusedElement, &pCurrentNode);
    
    int count = 0;
    while (pCurrentNode && count < maxSiblings) {
        contextList.push_back(ExtractElementProperties(pCurrentNode));
        
        IUIAutomationElement* pNext = nullptr;
        pTreeWalker->GetNextSiblingElement(pCurrentNode, &pNext);
        pCurrentNode->Release();
        pCurrentNode = pNext;
        count++;
    }

    pTreeWalker->GetPreviousSiblingElement(pFocusedElement, &pCurrentNode);
    count = 0;
    while (pCurrentNode && count < maxSiblings) {
        contextList.insert(contextList.begin(), ExtractElementProperties(pCurrentNode));
        
        IUIAutomationElement* pPrev = nullptr;
        pTreeWalker->GetPreviousSiblingElement(pCurrentNode, &pPrev);
        pCurrentNode->Release();
        pCurrentNode = pPrev;
        count++;
    }

    pTreeWalker->Release();
    return contextList;
}

std::string JsonEscapeSafe(const std::string& input) {
    std::string output;
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\b') output += "\\b";
        else if (c == '\f') output += "\\f";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else output += c;
    }
    return output;
}

std::string SerializeUIAContextMatrix(const std::vector<UIAContextMatrix>& matrixList) {
    std::string json = "[";
    for (size_t i = 0; i < matrixList.size(); ++i) {
        json += "{";
        json += "\"automationId\":\"" + JsonEscapeSafe(WideToUtf8(matrixList[i].automationId)) + "\",";
        json += "\"valueText\":\"" + JsonEscapeSafe(WideToUtf8(matrixList[i].valueText)) + "\",";
        json += "\"rangeText\":\"" + JsonEscapeSafe(WideToUtf8(matrixList[i].rangeText)) + "\",";
        json += "\"legacyValue\":\"" + JsonEscapeSafe(WideToUtf8(matrixList[i].legacyValue)) + "\"";
        json += "}";
        if (i < matrixList.size() - 1) json += ",";
    }
    json += "]";
    return json;
}

// Dedicated STA thread proc for UIA operations
DWORD WINAPI UIAThreadProc(LPVOID lpParam) {
    g_staThreadId = GetCurrentThreadId();
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

    hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&g_pAutomation));
    if (FAILED(hr)) { CoUninitialize(); return 1; }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        // Custom message to process a queued HWND
        if (msg.message == WM_USER + 1) {
            HWND hwnd = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                if (!g_hwndQueue.empty()) {
                    hwnd = g_hwndQueue.front();
                    g_hwndQueue.pop();
                }
            }
            if (hwnd) ProcessUIAEvent(hwnd);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_pAutomation) { g_pAutomation->Release(); g_pAutomation = nullptr; }
    CoUninitialize();
    return 0;
}

extern std::wstring ExtractTextFromHWND(HWND targetHwnd);

// Executes a targeted query against the focused HWND (runs on STA thread)
void ProcessUIAEvent(HWND hwnd) {
    if (!g_pAutomation || !hwnd) return;

    IUIAutomationElement* pElement = nullptr;
    HRESULT hr = g_pAutomation->ElementFromHandle(hwnd, &pElement);
    if (FAILED(hr) || !pElement) return;

    ApplicationStateNode node;
    node.hwnd = hwnd;
    GetWindowRect(hwnd, &node.windowBounds);
    GetWindowThreadProcessId(hwnd, &node.processId);

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, node.processId);
    if (hProcess) {
        WCHAR processName[MAX_PATH] = {0};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, processName, &size)) {
            node.processName = processName;
        }
        CloseHandle(hProcess);
    }

    BSTR name = nullptr, className = nullptr;
    pElement->get_CurrentName(&name);
    pElement->get_CurrentClassName(&className);

    BOOL isPassword = FALSE;
    VARIANT var;
    if (SUCCEEDED(pElement->GetCurrentPropertyValue(UIA_IsPasswordPropertyId, &var))) {
        if (var.vt == VT_BOOL && var.boolVal == VARIANT_TRUE) {
            isPassword = TRUE;
        }
        VariantClear(&var);
    }

    // Heuristic: Check name for password/pin
    if (!isPassword && name) {
        std::wstring uiaName = name;
        std::wstring lowerName = uiaName;
        for (auto& c : lowerName) c = towlower(c);
        if (lowerName.find(L"password") != std::wstring::npos ||
            lowerName.find(L"passphrase") != std::wstring::npos ||
            lowerName.find(L"pin") != std::wstring::npos) {
            isPassword = TRUE;
        }
    }

    if (isPassword) {
        node.activeElementUiaName = L"[REDACTED_PASSWORD_FIELD]";
        node.activeElementUiaType = className ? className : L"";
        node.extractedOcrText = L""; // Explicitly block OCR
    } else {
        if (name) { node.activeElementUiaName = RedactPII(name); }
        if (className) { node.activeElementUiaType = RedactPII(className); }

        // OCR Fallback if UIA gives no useful text
        if (node.activeElementUiaName.empty()) {
            HKEY hKey;
            DWORD disableOcr = 0;
            DWORD size = sizeof(DWORD);
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\CompatRuntime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                RegQueryValueExW(hKey, L"DisableOCR", NULL, NULL, (LPBYTE)&disableOcr, &size);
                RegCloseKey(hKey);
            }
            if (!disableOcr) {
                node.extractedOcrText = RedactPII(ExtractTextFromHWND(hwnd));
            }
        }
    }

    if (!isPassword) {
        node.visualAnchor = CaptureLocalizedVisualAnchor(pElement);
        std::vector<UIAContextMatrix> siblingMatrix = BuildSiblingContextMatrix(g_pAutomation, pElement, 3);
        node.uiaMatrixJson = SerializeUIAContextMatrix(siblingMatrix);
    }

    if (name) SysFreeString(name);
    if (className) SysFreeString(className);

    node.timestamp = GetTickCount64();
    OnApplicationStateChanged(node);
    pElement->Release();
}

// Global WinEvent callback - runs on main thread, forwards to STA via PostThreadMessage
void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW) return;
    if (event == EVENT_SYSTEM_FOREGROUND || event == EVENT_OBJECT_FOCUS) {
        if (g_staThreadId) {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_hwndQueue.push(hwnd);
            PostThreadMessage(g_staThreadId, WM_USER + 1, 0, 0);
        }
    }
}

void RegisterEventHooks() {
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
                    WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, NULL,
                    WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL,
                    WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void InitializeUIA() {
    CreateThread(NULL, 0, UIAThreadProc, NULL, 0, NULL);
}

void ShutdownUIA() {
    if (g_staThreadId) PostThreadMessage(g_staThreadId, WM_QUIT, 0, 0);
    if (g_pAutomation) { g_pAutomation->Release(); g_pAutomation = nullptr; }
    CoUninitialize();
}
