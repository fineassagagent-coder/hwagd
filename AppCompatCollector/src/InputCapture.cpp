#include "hwagd_structs.h"
#include <iostream>
#include <vector>

extern void OnUserActionIntercepted(const UserActionEdge& edge);

// Processes dead-key sequences and handles IME fallback
WCHAR ProcessComplexKeystroke(UINT vk, UINT makeCode, BYTE* keyboardState) {
    WCHAR buffer[4] = {0};
    int result = ToUnicode(vk, makeCode, keyboardState, buffer, 4, 0);
    if (result > 0) return buffer[0];                 // single character
    if (result == 2) {                                 // dead key - consume second call
        result = ToUnicode(vk, makeCode, keyboardState, buffer, 4, 0);
        if (result > 0) return buffer[0];
    }
    return L'\0'; // IME composition - fall back to UIA TextPattern
}

LRESULT CALLBACK MessageOnlyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) {
        UINT dwSize = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
        std::vector<BYTE> lpb(dwSize);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize)
            return DefWindowProc(hwnd, msg, wParam, lParam);

        RAWINPUT* raw = (RAWINPUT*)lpb.data();
        if (raw->header.dwType == RIM_TYPEKEYBOARD && raw->data.keyboard.Message == WM_KEYDOWN) {
            BYTE keyboardState[256];
            GetKeyboardState(keyboardState);
            WCHAR ch = ProcessComplexKeystroke(raw->data.keyboard.VKey, raw->data.keyboard.MakeCode, keyboardState);
            if (ch != L'\0') {
                UserActionEdge edge;
                edge.actionType = L"KEYSTROKE";
                edge.actionPayload = std::wstring(1, ch);
                edge.sourceTimestamp = GetTickCount64();
                edge.destTimestamp = 0;
                OnUserActionIntercepted(edge);
            }
            // If ch == L'\0', IME is active - UIA TextPattern will capture finalised text separately
        }
        else if (raw->header.dwType == RIM_TYPEMOUSE && raw->data.mouse.usButtonFlags != 0) {
            UserActionEdge edge;
            if (raw->data.mouse.usButtonFlags & RI_MOUSE_WHEEL) {
                edge.actionType = L"SCROLL_DELTA";
                edge.actionPayload = std::to_wstring((short)raw->data.mouse.usButtonData);
            } else {
                edge.actionType = L"MOUSE_CLICK";
                edge.actionPayload = std::to_wstring(raw->data.mouse.usButtonFlags);
            }
            edge.sourceTimestamp = GetTickCount64();
            edge.destTimestamp = 0;
            OnUserActionIntercepted(edge);
        }
        return 0;
    }
    else if (msg == WM_CLIPBOARDUPDATE) {
        UserActionEdge edge;
        edge.actionType = L"CLIPBOARD_MUTATION";
        edge.actionPayload = std::to_wstring(GetClipboardSequenceNumber());
        edge.sourceTimestamp = GetTickCount64();
        edge.destTimestamp = 0;
        OnUserActionIntercepted(edge);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void InitializeRawInputEngine() {
    WNDCLASSEX wx = {0};
    wx.cbSize = sizeof(WNDCLASSEX);
    wx.lpfnWndProc = MessageOnlyWndProc;
    wx.lpszClassName = L"HwagdMessageSinkClass";
    RegisterClassEx(&wx);

    HWND hwndMessage = CreateWindowEx(0, L"HwagdMessageSinkClass", L"HwagdSink",
                                      0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    if (!hwndMessage) return;

    ChangeWindowMessageFilterEx(hwndMessage, WM_INPUT, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(hwndMessage, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    AddClipboardFormatListener(hwndMessage);

    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02; // Mouse
    rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwndMessage;

    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06; // Keyboard
    rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = hwndMessage;

    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
}
