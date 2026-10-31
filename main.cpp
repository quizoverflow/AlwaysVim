#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"VimKeyOverlayWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerIntervalMs = 250;
constexpr ULONGLONG kGPrefixTimeoutMs = 500;
constexpr ULONGLONG kYankPrefixTimeoutMs = 1000;
constexpr ULONGLONG kClipboardWaitMs = 1500;
constexpr int kOverlayWidth = 224;
constexpr int kOverlayHeight = 48;
constexpr int kOverlayMargin = 18;
constexpr int kButtonSize = 28;
constexpr int kButtonTop = (kOverlayHeight - kButtonSize) / 2;
constexpr int kPauseButtonLeft = 153;
constexpr int kCloseButtonLeft = 188;

enum class Mode { Insert, Normal, Visual };
enum class YankKind { None, Character, Line };

struct AppState {
    HWND window = nullptr;
    std::array<HWND, 2> buttons{};
    HHOOK keyboardHook = nullptr;
    Mode mode = Mode::Insert;
    bool enabled = true;
    bool pendingG = false;
    bool gKeyDown = false;
    bool pendingYank = false;
    bool yKeyDown = false;
    bool visualSelectionStarted = false;
    bool leftShiftDown = false;
    bool rightShiftDown = false;
    bool toggleHotkeyDown = false;
    bool exitHotkeyDown = false;
    ULONGLONG gPrefixStartedAt = 0;
    ULONGLONG yankPrefixStartedAt = 0;
    YankKind pendingClipboardYank = YankKind::None;
    YankKind clipboardYank = YankKind::None;
    DWORD clipboardSequence = 0;
    ULONGLONG clipboardYankStartedAt = 0;
    std::array<bool, 256> swallowedKeyUps{};
};

AppState g_app;

bool IsKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void SendChord(WORD key, bool ctrl = false, bool shift = false) {
    std::vector<INPUT> inputs;
    inputs.reserve(12);

    auto addKey = [&inputs](WORD vk, bool isDown) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (scan) {
            input.ki.wScan = static_cast<WORD>(scan);
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
        } else {
            input.ki.wVk = vk;
        }
        switch (vk) {
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_INSERT: case VK_DELETE:
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                break;
        }
        if (!isDown) input.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(input);
    };

    // Physical Shift may still be held for commands such as G or $.
    const bool leftShiftDown = g_app.leftShiftDown;
    const bool rightShiftDown = g_app.rightShiftDown;
    const bool shiftDown = leftShiftDown || rightShiftDown;
    if (!shift && leftShiftDown) addKey(VK_LSHIFT, false);
    if (!shift && rightShiftDown) addKey(VK_RSHIFT, false);
    if (ctrl) addKey(VK_LCONTROL, true);
    if (shift && !shiftDown) addKey(VK_LSHIFT, true);
    addKey(key, true);
    addKey(key, false);
    if (shift && !shiftDown) addKey(VK_LSHIFT, false);
    if (ctrl) addKey(VK_LCONTROL, false);
    if (!shift && leftShiftDown) addKey(VK_LSHIFT, true);
    if (!shift && rightShiftDown) addKey(VK_RSHIFT, true);

    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void RefreshClipboardYank() {
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == 0) return;
    if (sequence != g_app.clipboardSequence) {
        g_app.clipboardYank = g_app.pendingClipboardYank;
        g_app.pendingClipboardYank = YankKind::None;
        g_app.clipboardSequence = sequence;
    } else if (g_app.pendingClipboardYank != YankKind::None &&
               GetTickCount64() - g_app.clipboardYankStartedAt > kClipboardWaitMs) {
        g_app.pendingClipboardYank = YankKind::None;
    }
}

void CopySelection(YankKind kind, bool collapseSelection) {
    RefreshClipboardYank();
    g_app.clipboardYank = YankKind::None;
    g_app.pendingClipboardYank = kind;
    g_app.clipboardYankStartedAt = GetTickCount64();
    SendChord('C', true);
    if (collapseSelection) SendChord(VK_LEFT);
}

bool LineClipboardEndsWithNewline(bool& endsWithNewline) {
    if (!OpenClipboard(g_app.window)) return false;
    bool available = false;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text) {
            const std::size_t capacity = GlobalSize(data) / sizeof(wchar_t);
            std::size_t length = 0;
            while (length < capacity && text[length] != L'\0') ++length;
            if (length < capacity) {
                endsWithNewline = length > 0 &&
                                  (text[length - 1] == L'\n' || text[length - 1] == L'\r');
                available = true;
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return available;
}

void PasteAfterCursor() {
    RefreshClipboardYank();
    bool endsWithNewline = false;
    if (g_app.clipboardYank == YankKind::Line &&
        LineClipboardEndsWithNewline(endsWithNewline)) {
        SendChord(VK_END);
        SendChord(VK_RETURN);
        SendChord('V', true);
        if (endsWithNewline) SendChord(VK_BACK);
        return;
    }
    SendChord(VK_RIGHT);
    SendChord('V', true);
}

void SetMode(Mode mode) {
    g_app.mode = mode;
    if (g_app.window) {
        InvalidateRect(g_app.window, nullptr, TRUE);
        UpdateWindow(g_app.window);
    }
}

void ClearPendingCommands() {
    g_app.pendingG = false;
    g_app.pendingYank = false;
}

const wchar_t* ModeText() {
    if (!g_app.enabled) return L"-- PAUSED --";
    switch (g_app.mode) {
        case Mode::Insert: return L"-- INSERT --";
        case Mode::Normal: return L"-- NORMAL --";
        case Mode::Visual: return L"-- VISUAL --";
    }
    return L"-- INSERT --";
}

COLORREF ModeColor() {
    if (!g_app.enabled) return RGB(145, 145, 145);
    switch (g_app.mode) {
        case Mode::Insert: return RGB(235, 235, 235);
        case Mode::Normal: return RGB(110, 225, 145);
        case Mode::Visual: return RGB(255, 190, 85);
    }
    return RGB(235, 235, 235);
}

void PositionOverlay() {
    if (!g_app.window) return;

    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;

    const int x = info.rcWork.left + kOverlayMargin;
    const int y = info.rcWork.bottom - kOverlayHeight - kOverlayMargin;
    SetWindowPos(g_app.window, HWND_TOPMOST, x, y, kOverlayWidth, kOverlayHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (g_app.buttons[0]) {
        SetWindowPos(g_app.buttons[0], HWND_TOPMOST, x + kPauseButtonLeft,
                     y + kButtonTop, kButtonSize, kButtonSize,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (g_app.buttons[1]) {
        SetWindowPos(g_app.buttons[1], HWND_TOPMOST, x + kCloseButtonLeft,
                     y + kButtonTop, kButtonSize, kButtonSize,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void PaintOverlay(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);

    RECT rect{};
    GetClientRect(window, &rect);
    HBRUSH background = CreateSolidBrush(RGB(28, 30, 34));
    FillRect(dc, &rect, background);
    DeleteObject(background);

    HPEN border = CreatePen(PS_SOLID, 1, RGB(74, 78, 84));
    HGDIOBJ oldPen = SelectObject(dc, border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 10, 10);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ModeColor());
    HFONT font = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    rect.right = kPauseButtonLeft - 4;
    DrawTextW(dc, ModeText(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    EndPaint(window, &paint);
}

void UpdateOverlay();

LRESULT CALLBACK ButtonWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            SetCapture(window);
            return 0;
        case WM_PAINT: {
            const bool isClose = GetWindowLongPtrW(window, GWLP_USERDATA) != 0;
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);

            RECT rect{};
            GetClientRect(window, &rect);
            HBRUSH background = CreateSolidBrush(RGB(43, 46, 52));
            FillRect(dc, &rect, background);
            DeleteObject(background);

            HPEN border = CreatePen(PS_SOLID, 1, RGB(82, 87, 95));
            HGDIOBJ oldPen = SelectObject(dc, border);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 8, 8);

            HPEN icon = CreatePen(PS_SOLID, 2, RGB(225, 228, 232));
            SelectObject(dc, icon);
            if (isClose) {
                MoveToEx(dc, 9, 9, nullptr);
                LineTo(dc, 19, 19);
                MoveToEx(dc, 19, 9, nullptr);
                LineTo(dc, 9, 19);
            } else if (g_app.enabled) {
                MoveToEx(dc, 9, 8, nullptr);
                LineTo(dc, 9, 20);
                MoveToEx(dc, 18, 8, nullptr);
                LineTo(dc, 18, 20);
            } else {
                POINT play[] = {{10, 7}, {20, 14}, {10, 21}, {10, 7}};
                Polyline(dc, play, 4);
            }

            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(icon);
            DeleteObject(border);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (GetCapture() == window) ReleaseCapture();
            const POINT point{static_cast<short>(LOWORD(lParam)),
                              static_cast<short>(HIWORD(lParam))};
            RECT rect{};
            GetClientRect(window, &rect);
            if (!PtInRect(&rect, point)) return 0;

            if (GetWindowLongPtrW(window, GWLP_USERDATA) != 0) {
                if (g_app.window) PostMessageW(g_app.window, WM_CLOSE, 0, 0);
            } else {
                g_app.enabled = !g_app.enabled;
                ClearPendingCommands();
                UpdateOverlay();
            }
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

void UpdateOverlay() {
    if (g_app.window) {
        InvalidateRect(g_app.window, nullptr, TRUE);
        UpdateWindow(g_app.window);
        for (HWND button : g_app.buttons) {
            if (button) {
                InvalidateRect(button, nullptr, TRUE);
                UpdateWindow(button);
            }
        }
        PositionOverlay();
    }
}

bool IsTextKey(DWORD vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return true;
    switch (vk) {
        case VK_SPACE:
        case VK_OEM_1:
        case VK_OEM_PLUS:
        case VK_OEM_COMMA:
        case VK_OEM_MINUS:
        case VK_OEM_PERIOD:
        case VK_OEM_2:
        case VK_OEM_3:
        case VK_OEM_4:
        case VK_OEM_5:
        case VK_OEM_6:
        case VK_OEM_7:
        case VK_OEM_102:
            return true;
        default:
            return false;
    }
}

void ConsumeKey(DWORD vk) {
    if (vk < g_app.swallowedKeyUps.size()) g_app.swallowedKeyUps[vk] = true;
}

bool HandleYankMotion(DWORD vk, bool shifted) {
    if (vk == 'G' && !shifted && !g_app.pendingG) {
        g_app.pendingG = true;
        g_app.gPrefixStartedAt = GetTickCount64();
        return true;
    }

    g_app.pendingYank = false;
    if (vk == 'Y' && !shifted) {
        SendChord(VK_HOME);
        SendChord(VK_END, false, true);
        SendChord(VK_RIGHT, false, true);
        CopySelection(YankKind::Line, true);
        return true;
    }

    WORD key = 0;
    bool ctrl = false;
    switch (vk) {
        case 'H': if (!shifted) key = VK_LEFT; break;
        case 'J': if (!shifted) key = VK_DOWN; break;
        case 'K': if (!shifted) key = VK_UP; break;
        case 'L': if (!shifted) key = VK_RIGHT; break;
        case 'W': if (!shifted) { key = VK_RIGHT; ctrl = true; } break;
        case 'B': if (!shifted) { key = VK_LEFT; ctrl = true; } break;
        case '0': if (!shifted) key = VK_HOME; break;
        case '4': if (shifted) key = VK_END; break;
        case '6': if (shifted) key = VK_HOME; break;
        case 'G':
            if (shifted) {
                key = VK_END;
                ctrl = true;
            } else if (g_app.pendingG &&
                       GetTickCount64() - g_app.gPrefixStartedAt <= kGPrefixTimeoutMs) {
                key = VK_HOME;
                ctrl = true;
            }
            g_app.pendingG = false;
            break;
    }
    if (key) {
        SendChord(key, ctrl, true);
        CopySelection(YankKind::Character, true);
    }
    return true;
}

bool HandleNormalKey(DWORD vk, bool shifted) {
    switch (vk) {
        case 'I':
            if (shifted) SendChord(VK_HOME);
            SetMode(Mode::Insert);
            return true;
        case 'A':
            SendChord(shifted ? VK_END : VK_RIGHT);
            SetMode(Mode::Insert);
            return true;
        case 'V':
            g_app.visualSelectionStarted = false;
            SetMode(Mode::Visual);
            return true;
        case 'Y':
            if (shifted) return false;
            g_app.pendingYank = true;
            g_app.yankPrefixStartedAt = GetTickCount64();
            return true;
        case 'P':
            if (shifted) return false;
            PasteAfterCursor();
            return true;
        case 'H': SendChord(VK_LEFT); return true;
        case 'J': SendChord(VK_DOWN); return true;
        case 'K': SendChord(VK_UP); return true;
        case 'L': SendChord(VK_RIGHT); return true;
        case 'O':
            if (shifted) {
                SendChord(VK_HOME);
                SendChord(VK_RETURN);
                SendChord(VK_UP);
            } else {
                SendChord(VK_END);
                SendChord(VK_RETURN);
            }
            SetMode(Mode::Insert);
            return true;
        case 'W': SendChord(VK_RIGHT, true); return true;
        case 'B': SendChord(VK_LEFT, true); return true;
        case 'G':
            if (shifted) {
                g_app.pendingG = false;
                SendChord(VK_END, true);
            } else if (g_app.pendingG && GetTickCount64() - g_app.gPrefixStartedAt <= kGPrefixTimeoutMs) {
                g_app.pendingG = false;
                SendChord(VK_HOME, true);
            } else {
                g_app.pendingG = true;
                g_app.gPrefixStartedAt = GetTickCount64();
            }
            return true;
        case '0': SendChord(VK_HOME); return true;
        case '4':
            if (shifted) {
                SendChord(VK_END);
                return true;
            }
            return false;
        case '6':
            if (shifted) {
                SendChord(VK_HOME);
                return true;
            }
            return false;
        case VK_OEM_3:
            // On US layouts, Shift+` produces ~; kept unmapped intentionally.
            return false;
        default:
            return false;
    }
}

void SendVisualMove(WORD key, bool ctrl = false) {
    SendChord(key, ctrl, true);
    g_app.visualSelectionStarted = true;
}

bool HandleVisualKey(DWORD vk, bool shifted) {
    switch (vk) {
        case 'H': SendVisualMove(VK_LEFT); return true;
        case 'J': SendVisualMove(VK_DOWN); return true;
        case 'K': SendVisualMove(VK_UP); return true;
        case 'L': SendVisualMove(VK_RIGHT); return true;
        case 'W': SendVisualMove(VK_RIGHT, true); return true;
        case 'B': SendVisualMove(VK_LEFT, true); return true;
        case 'Y':
            if (shifted) return false;
            CopySelection(YankKind::Character, g_app.visualSelectionStarted);
            g_app.visualSelectionStarted = false;
            SetMode(Mode::Normal);
            return true;
        case 'G':
            if (shifted) {
                g_app.pendingG = false;
                SendVisualMove(VK_END, true);
            } else if (g_app.pendingG && GetTickCount64() - g_app.gPrefixStartedAt <= kGPrefixTimeoutMs) {
                g_app.pendingG = false;
                SendVisualMove(VK_HOME, true);
            } else {
                g_app.pendingG = true;
                g_app.gPrefixStartedAt = GetTickCount64();
            }
            return true;
        case '0': SendVisualMove(VK_HOME); return true;
        case '4':
            if (shifted) {
                SendVisualMove(VK_END);
                return true;
            }
            return false;
        case '6':
            if (shifted) {
                SendVisualMove(VK_HOME);
                return true;
            }
            return false;
        default:
            return false;
    }
}

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM message, LPARAM data) {
    if (code != HC_ACTION || !g_app.window) {
        return CallNextHookEx(g_app.keyboardHook, code, message, data);
    }

    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    const DWORD vk = key->vkCode;
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!keyDown && !keyUp) return CallNextHookEx(g_app.keyboardHook, code, message, data);

    // Simulated keystrokes from SendInput must continue to the target application.
    if (key->flags & LLKHF_INJECTED) {
        return CallNextHookEx(g_app.keyboardHook, code, message, data);
    }

    if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT) {
        const bool right = vk == VK_RSHIFT ||
                           (vk == VK_SHIFT && key->scanCode == MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC));
        if (right) g_app.rightShiftDown = keyDown;
        else g_app.leftShiftDown = keyDown;
    }

    if (keyUp) {
        if (vk == 'G') g_app.gKeyDown = false;
        if (vk == 'Y') g_app.yKeyDown = false;
        if (vk == VK_F12) g_app.toggleHotkeyDown = false;
        if (vk == VK_F11) g_app.exitHotkeyDown = false;
        if (vk < g_app.swallowedKeyUps.size() && g_app.swallowedKeyUps[vk]) {
            g_app.swallowedKeyUps[vk] = false;
            return 1;
        }
        return CallNextHookEx(g_app.keyboardHook, code, message, data);
    }

    const bool ctrl = IsKeyDown(VK_CONTROL);
    const bool alt = IsKeyDown(VK_MENU);
    const bool shift = g_app.leftShiftDown || g_app.rightShiftDown;

    if (vk == VK_F12 && ctrl && alt) {
        if (!g_app.toggleHotkeyDown) {
            g_app.enabled = !g_app.enabled;
            ClearPendingCommands();
            g_app.toggleHotkeyDown = true;
            UpdateOverlay();
        }
        ConsumeKey(vk);
        return 1;
    }
    if (vk == VK_F11 && ctrl && alt && shift) {
        if (!g_app.exitHotkeyDown) {
            g_app.exitHotkeyDown = true;
            PostMessageW(g_app.window, WM_CLOSE, 0, 0);
        }
        ConsumeKey(vk);
        return 1;
    }

    if (!g_app.enabled || ctrl || alt) {
        ClearPendingCommands();
        return CallNextHookEx(g_app.keyboardHook, code, message, data);
    }

    if (g_app.mode == Mode::Insert) {
        if (vk == VK_ESCAPE) {
            SetMode(Mode::Normal);
            ConsumeKey(vk);
            return 1;
        }
        return CallNextHookEx(g_app.keyboardHook, code, message, data);
    }

    // Shift is a modifier for commands such as yG and y$, not the motion itself.
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        return CallNextHookEx(g_app.keyboardHook, code, message, data);
    }

    if (vk == 'G') {
        if (g_app.gKeyDown) {
            ConsumeKey(vk);
            return 1;
        }
        g_app.gKeyDown = true;
    } else if (g_app.pendingG) {
        if (g_app.pendingYank) g_app.pendingYank = false;
        g_app.pendingG = false;
    }

    if (vk == 'Y') {
        if (g_app.yKeyDown) {
            ConsumeKey(vk);
            return 1;
        }
        g_app.yKeyDown = true;
    }

    if (vk == VK_ESCAPE) {
        ClearPendingCommands();
        if (g_app.mode == Mode::Visual) {
            if (g_app.visualSelectionStarted) SendChord(VK_LEFT);
            g_app.visualSelectionStarted = false;
            SetMode(Mode::Normal);
        }
        ConsumeKey(vk);
        return 1;
    }

    if (g_app.pendingYank) {
        if (GetTickCount64() - g_app.yankPrefixStartedAt <= kYankPrefixTimeoutMs) {
            HandleYankMotion(vk, shift);
            ConsumeKey(vk);
            return 1;
        }
        ClearPendingCommands();
    }

    const bool handled = g_app.mode == Mode::Normal
                             ? HandleNormalKey(vk, shift)
                             : HandleVisualKey(vk, shift);
    if (handled || IsTextKey(vk)) {
        ConsumeKey(vk);
        return 1;
    }
    return CallNextHookEx(g_app.keyboardHook, code, message, data);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            PaintOverlay(window);
            return 0;
        case WM_TIMER:
            if (g_app.pendingG && GetTickCount64() - g_app.gPrefixStartedAt > kGPrefixTimeoutMs) {
                g_app.pendingG = false;
                g_app.pendingYank = false;
            }
            if (g_app.pendingYank &&
                GetTickCount64() - g_app.yankPrefixStartedAt > kYankPrefixTimeoutMs) {
                ClearPendingCommands();
            }
            RefreshClipboardYank();
            PositionOverlay();
            return 0;
        case WM_CLIPBOARDUPDATE:
            RefreshClipboardYank();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kTimerId);
            RemoveClipboardFormatListener(window);
            for (HWND& button : g_app.buttons) {
                if (button) {
                    DestroyWindow(button);
                    button = nullptr;
                }
            }
            if (g_app.keyboardHook) {
                UnhookWindowsHookEx(g_app.keyboardHook);
                g_app.keyboardHook = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return 1;

    WNDCLASSEXW buttonClass{};
    buttonClass.cbSize = sizeof(buttonClass);
    buttonClass.lpfnWndProc = ButtonWindowProc;
    buttonClass.hInstance = instance;
    buttonClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32649));
    buttonClass.lpszClassName = L"VimKeyOverlayButtonWindow";
    if (!RegisterClassExW(&buttonClass)) return 1;

    const DWORD extendedStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT |
                                WS_EX_LAYERED | WS_EX_NOACTIVATE;
    g_app.window = CreateWindowExW(extendedStyle, kWindowClass, L"Vim Key Overlay",
                                   WS_POPUP, 0, 0, kOverlayWidth, kOverlayHeight,
                                   nullptr, nullptr, instance, nullptr);
    if (!g_app.window) return 1;
    g_app.clipboardSequence = GetClipboardSequenceNumber();
    AddClipboardFormatListener(g_app.window);

    const DWORD buttonStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
                              WS_EX_NOACTIVATE;
    for (std::size_t i = 0; i < g_app.buttons.size(); ++i) {
        g_app.buttons[i] = CreateWindowExW(
            buttonStyle, buttonClass.lpszClassName,
            i == 0 ? L"Pause or resume Vim mode" : L"Exit VimAgent",
            WS_POPUP, 0, 0,
            kButtonSize, kButtonSize, nullptr, nullptr, instance,
            reinterpret_cast<LPVOID>(static_cast<INT_PTR>(i)));
        if (!g_app.buttons[i]) {
            DestroyWindow(g_app.window);
            return 1;
        }
        SetLayeredWindowAttributes(g_app.buttons[i], 0, 232, LWA_ALPHA);
    }

    SetLayeredWindowAttributes(g_app.window, 0, 232, LWA_ALPHA);
    PositionOverlay();
    SetTimer(g_app.window, kTimerId, kTimerIntervalMs, nullptr);

    g_app.leftShiftDown = IsKeyDown(VK_LSHIFT);
    g_app.rightShiftDown = IsKeyDown(VK_RSHIFT);
    g_app.keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, instance, 0);
    if (!g_app.keyboardHook) {
        DestroyWindow(g_app.window);
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
