#include <Windows.h>
#include "pch.h"
#include <string>
#include <chrono>
#include <cmath>

#pragma comment(lib, "user32.lib")

// Globals
HWND hwndOverlay = nullptr;
bool running = true;

bool menuOpen = false;

bool crosshairEnabled = true;
int crosshairSize = 15;
int crosshairGap = 5;

enum CrosshairShape { SHAPE_PLUS, SHAPE_CIRCLE, SHAPE_DOT, SHAPE_CROSS };
CrosshairShape crosshairShape = SHAPE_PLUS;

int colorR = 0, colorG = 160, colorB = 255;
bool rainbowEnabled = false;

bool watermarkEnabled = true;

// --- NEW: Scope Overlay Options ---
bool scopeOverlayEnabled = false;
int scopeRadius = 100;
int scopeOffsetX = 0;
int scopeOffsetY = 0;

// --- NEW: Kill Effect ---
struct KillEffect
{
    bool active = false;
    DWORD startTime = 0;
    int duration = 1500; // milliseconds
    int x = 0, y = 0; // position with offset

    void Start(int posX, int posY)
    {
        active = true;
        startTime = GetTickCount();
        x = posX;
        y = posY;
    }

    void Draw(HDC hdc)
    {
        if (!active) return;

        DWORD now = GetTickCount();
        DWORD elapsed = now - startTime;
        if (elapsed > duration)
        {
            active = false;
            return;
        }

        float progress = elapsed / (float)duration;
        BYTE alpha = (BYTE)(255 * (1.0f - progress)); // fade out

        SetBkMode(hdc, TRANSPARENT);

        // Draw shadow
        SetTextColor(hdc, RGB(0, 0, 0));
        SetTextCharacterExtra(hdc, 2);
        TextOutA(hdc, x + 2, y + 2, "KILL!", 5);

        // Draw main text with alpha-like effect (GDI can't do real alpha text, so just solid)
        COLORREF mainColor = RGB(255, 50, 50);
        SetTextColor(hdc, mainColor);
        TextOutA(hdc, x, y, "KILL!", 5);
    }
} killEffect;

COLORREF blueMain = RGB(0, 160, 255);
COLORREF blueDark = RGB(0, 90, 140);
COLORREF whiteColor = RGB(255, 255, 255);
COLORREF grayColor = RGB(128, 128, 128);

SHORT lastInsertState = 0;
SHORT lastUpState = 0;
SHORT lastDownState = 0;
SHORT lastLeftState = 0;
SHORT lastRightState = 0;
SHORT lastEnterState = 0;
SHORT lastKState = 0;

int menuSelection = 0; // menu item index
const int MENU_ITEM_COUNT = 12;

int width = 2560;
int height = 1440;

// FPS tracking
DWORD lastTick = 0;
int frameCount = 0;
float currentFPS = 0.0f;

// Drawing buffer
HDC hMemDC = nullptr;
HBITMAP hBitmap = nullptr;
BYTE* pBits = nullptr;

// Helpers

COLORREF HSVtoRGB(float h, float s, float v)
{
    float r, g, b;
    int i = (int)(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    switch (i % 6)
    {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    case 5: r = v, g = p, b = q; break;
    }
    return RGB((BYTE)(r * 255), (BYTE)(g * 255), (BYTE)(b * 255));
}

void DrawTextWithShadow(HDC hdc, int x, int y, const char* text, COLORREF color)
{
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, x + 1, y + 1, text, (int)strlen(text));
    SetTextColor(hdc, color);
    TextOutA(hdc, x, y, text, (int)strlen(text));
}

void DrawRoundedRect(HDC hdc, RECT rect, COLORREF color, int radius)
{
    HBRUSH brush = CreateSolidBrush(color);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    DeleteObject(brush);
}

void DrawCrosshair(HDC hdc, int cx, int cy, COLORREF color, int size, int gap, CrosshairShape shape)
{
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);

    switch (shape)
    {
    case SHAPE_PLUS:
        MoveToEx(hdc, cx - size, cy, nullptr);
        LineTo(hdc, cx - gap, cy);
        MoveToEx(hdc, cx + gap, cy, nullptr);
        LineTo(hdc, cx + size, cy);
        MoveToEx(hdc, cx, cy - size, nullptr);
        LineTo(hdc, cx, cy - gap);
        MoveToEx(hdc, cx, cy + gap, nullptr);
        LineTo(hdc, cx, cy + size);
        break;
    case SHAPE_CIRCLE:
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Ellipse(hdc, cx - size, cy - size, cx + size, cy + size);
        break;
    case SHAPE_DOT:
    {
        HBRUSH brush = CreateSolidBrush(color);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
        Ellipse(hdc, cx - size / 4, cy - size / 4, cx + size / 4, cy + size / 4);
        SelectObject(hdc, oldBrush);
        DeleteObject(brush);
        break;
    }
    case SHAPE_CROSS:
        MoveToEx(hdc, cx - size, cy - size, nullptr);
        LineTo(hdc, cx - gap, cy - gap);
        MoveToEx(hdc, cx + gap, cy + gap, nullptr);
        LineTo(hdc, cx + size, cy + size);
        MoveToEx(hdc, cx - size, cy + size, nullptr);
        LineTo(hdc, cx - gap, cy + gap);
        MoveToEx(hdc, cx + gap, cy - gap, nullptr);
        LineTo(hdc, cx + size, cy - size);
        break;
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// --- NEW: Draw Scope Overlay ---
void DrawScopeOverlay(HDC hdc, int cx, int cy, int radius, int offsetX = 0, int offsetY = 0)
{
    cx += offsetX;
    cy += offsetY;

    // Draw dark vignette rings around scope (approximation)
    int vignetteThickness = 50;
    for (int i = 0; i < vignetteThickness; i += 10)
    {
        int alpha = 80 - i * 1; // decreasing alpha
        if (alpha < 0) alpha = 0;
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, (BYTE)alpha, AC_SRC_ALPHA };

        // GDI doesn't support alpha brush natively, so we fake by drawing transparent circle on DIB buffer directly
        // Since we're drawing on a layered window with UpdateLayeredWindow with per-pixel alpha, this approximation is good enough

        // Draw concentric circles with decreasing size and alpha (just dark circles)
        SelectObject(hdc, brush);
        SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, cx - radius - i, cy - radius - i, cx + radius + i, cy + radius + i);
        DeleteObject(brush);
    }

    // Draw white scope circle
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    // Draw cross lines inside scope
    HPEN linePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN oldLinePen = (HPEN)SelectObject(hdc, linePen);

    MoveToEx(hdc, cx, cy - radius, nullptr);
    LineTo(hdc, cx, cy + radius);

    MoveToEx(hdc, cx - radius, cy, nullptr);
    LineTo(hdc, cx + radius, cy);

    SelectObject(hdc, oldLinePen);
    DeleteObject(linePen);
}

void DrawWatermark(HDC hdc)
{
    static DWORD start = 0;
    if (!start) start = GetTickCount();
    float t = (GetTickCount() - start) / 1000.0f;

    // Pulsing alpha
    BYTE alpha = (BYTE)(165 + 65 * sinf(t * 3));

    int sway = (int)(5 * sinf(t * 2));

    COLORREF color = RGB(0, 160, 255);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, 11 + sway, 11, "Astral", 6);
    SetTextColor(hdc, color);
    TextOutA(hdc, 10 + sway, 10, "Astral", 6);
}

void DrawInfoPanel(HDC hdc)
{
    RECT panelRect = { 10, 40, 280, 170 };
    DrawRoundedRect(hdc, panelRect, RGB(0, 0, 0), 10);

    char buf[64];
    sprintf_s(buf, "FPS: %.1f", currentFPS);
    DrawTextWithShadow(hdc, 20, 50, buf, whiteColor);

    sprintf_s(buf, "Crosshair: %s", crosshairEnabled ? "ON" : "OFF");
    DrawTextWithShadow(hdc, 20, 70, buf, crosshairEnabled ? blueMain : grayColor);

    sprintf_s(buf, "Watermark: %s", watermarkEnabled ? "ON" : "OFF");
    DrawTextWithShadow(hdc, 20, 90, buf, watermarkEnabled ? blueMain : grayColor);

    sprintf_s(buf, "Scope Overlay: %s", scopeOverlayEnabled ? "ON" : "OFF");
    DrawTextWithShadow(hdc, 20, 110, buf, scopeOverlayEnabled ? blueMain : grayColor);

    sprintf_s(buf, "Scope Radius: %d", scopeRadius);
    DrawTextWithShadow(hdc, 20, 130, buf, blueMain);

    sprintf_s(buf, "Scope Offset X: %d", scopeOffsetX);
    DrawTextWithShadow(hdc, 20, 150, buf, blueMain);

    sprintf_s(buf, "Scope Offset Y: %d", scopeOffsetY);
    DrawTextWithShadow(hdc, 20, 170, buf, blueMain);
}

void ProcessInput()
{
    SHORT insertState = GetAsyncKeyState(VK_INSERT);
    SHORT upState = GetAsyncKeyState(VK_UP);
    SHORT downState = GetAsyncKeyState(VK_DOWN);
    SHORT leftState = GetAsyncKeyState(VK_LEFT);
    SHORT rightState = GetAsyncKeyState(VK_RIGHT);
    SHORT enterState = GetAsyncKeyState(VK_RETURN);
    SHORT kState = GetAsyncKeyState('K');

    if ((insertState & 0x8000) && !(lastInsertState & 0x8000))
    {
        menuOpen = !menuOpen;
        Sleep(150);
    }

    // Trigger kill effect demo when pressing K (only on key down)
    if ((kState & 0x8000) && !(lastKState & 0x8000))
    {
        int cx = width / 2;
        int cy = height / 2;
        killEffect.Start(cx + 50, cy - 50); // Demo position offset
    }
    lastKState = kState;

    if (!menuOpen) {
        lastInsertState = insertState;
        lastUpState = upState;
        lastDownState = downState;
        lastLeftState = leftState;
        lastRightState = rightState;
        lastEnterState = enterState;
        return;
    }

    if ((upState & 0x8000) && !(lastUpState & 0x8000))
    {
        menuSelection--;
        if (menuSelection < 0) menuSelection = MENU_ITEM_COUNT - 1;
        Sleep(120);
    }

    if ((downState & 0x8000) && !(lastDownState & 0x8000))
    {
        menuSelection++;
        if (menuSelection >= MENU_ITEM_COUNT) menuSelection = 0;
        Sleep(120);
    }

    switch (menuSelection)
    {
    case 0:
        if ((enterState & 0x8000) && !(lastEnterState & 0x8000))
        {
            crosshairEnabled = !crosshairEnabled;
            Sleep(150);
        }
        break;

    case 1:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000) && crosshairSize > 1)
        {
            crosshairSize--;
            Sleep(80);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000) && crosshairSize < 50)
        {
            crosshairSize++;
            Sleep(80);
        }
        break;

    case 2:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000) && crosshairGap > 0)
        {
            crosshairGap--;
            Sleep(80);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000) && crosshairGap < 20)
        {
            crosshairGap++;
            Sleep(80);
        }
        break;

    case 3:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000))
        {
            crosshairShape = (CrosshairShape)(((int)crosshairShape + 3) % 4);
            Sleep(80);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000))
        {
            crosshairShape = (CrosshairShape)(((int)crosshairShape + 1) % 4);
            Sleep(80);
        }
        break;

    case 4:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000) && colorR > 0)
        {
            colorR--;
            Sleep(60);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000) && colorR < 255)
        {
            colorR++;
            Sleep(60);
        }
        break;

    case 5:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000) && colorG > 0)
        {
            colorG--;
            Sleep(60);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000) && colorG < 255)
        {
            colorG++;
            Sleep(60);
        }
        break;

    case 6:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000) && colorB > 0)
        {
            colorB--;
            Sleep(60);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000) && colorB < 255)
        {
            colorB++;
            Sleep(60);
        }
        break;

    case 7:
        if ((enterState & 0x8000) && !(lastEnterState & 0x8000))
        {
            rainbowEnabled = !rainbowEnabled;
            Sleep(150);
        }
        break;

    case 8:
        if ((enterState & 0x8000) && !(lastEnterState & 0x8000))
        {
            watermarkEnabled = !watermarkEnabled;
            Sleep(150);
        }
        break;

    case 9:
        if ((enterState & 0x8000) && !(lastEnterState & 0x8000))
        {
            scopeOverlayEnabled = !scopeOverlayEnabled;
            Sleep(150);
        }
        break;

    case 10:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000) && scopeRadius > 10)
        {
            scopeRadius -= 5;
            Sleep(80);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000) && scopeRadius < 300)
        {
            scopeRadius += 5;
            Sleep(80);
        }
        break;

    case 11:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000))
        {
            scopeOffsetX -= 5;
            Sleep(80);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000))
        {
            scopeOffsetX += 5;
            Sleep(80);
        }
        break;

    case 12:
        if ((leftState & 0x8000) && !(lastLeftState & 0x8000))
        {
            scopeOffsetY -= 5;
            Sleep(80);
        }
        if ((rightState & 0x8000) && !(lastRightState & 0x8000))
        {
            scopeOffsetY += 5;
            Sleep(80);
        }
        break;
    }

    lastInsertState = insertState;
    lastUpState = upState;
    lastDownState = downState;
    lastLeftState = leftState;
    lastRightState = rightState;
    lastEnterState = enterState;
}

void DrawMenu(HDC hdc)
{
    RECT menuRect = { 50, 50, 400, 450 };
    DrawRoundedRect(hdc, menuRect, blueDark, 15);

    const char* crosshairShapeNames[] = { "Plus", "Circle", "Dot", "Cross" };
    char buf[64];

    SetBkMode(hdc, TRANSPARENT);
    DrawTextWithShadow(hdc, 60, 60, "Cheat Menu (Use Arrow Keys + Enter)", whiteColor);

    for (int i = 0; i < MENU_ITEM_COUNT; i++)
    {
        COLORREF color = (i == menuSelection) ? blueMain : whiteColor;
        switch (i)
        {
        case 0:
            sprintf_s(buf, "Crosshair: %s", crosshairEnabled ? "ON" : "OFF");
            break;
        case 1:
            sprintf_s(buf, "Crosshair Size: %d", crosshairSize);
            break;
        case 2:
            sprintf_s(buf, "Crosshair Gap: %d", crosshairGap);
            break;
        case 3:
            sprintf_s(buf, "Crosshair Shape: %s", crosshairShapeNames[(int)crosshairShape]);
            break;
        case 4:
            sprintf_s(buf, "Color R: %d", colorR);
            break;
        case 5:
            sprintf_s(buf, "Color G: %d", colorG);
            break;
        case 6:
            sprintf_s(buf, "Color B: %d", colorB);
            break;
        case 7:
            sprintf_s(buf, "Rainbow: %s", rainbowEnabled ? "ON" : "OFF");
            break;
        case 8:
            sprintf_s(buf, "Watermark: %s", watermarkEnabled ? "ON" : "OFF");
            break;
        case 9:
            sprintf_s(buf, "Scope Overlay: %s", scopeOverlayEnabled ? "ON" : "OFF");
            break;
        case 10:
            sprintf_s(buf, "Scope Radius: %d", scopeRadius);
            break;
        case 11:
            sprintf_s(buf, "Scope Offset X: %d", scopeOffsetX);
            break;
        case 12:
            sprintf_s(buf, "Scope Offset Y: %d", scopeOffsetY);
            break;
        }
        DrawTextWithShadow(hdc, 70, 90 + i * 25, buf, color);
    }
}

// Window procedure to do nothing (we don't use WM_PAINT anymore)
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY)
    {
        running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

DWORD WINAPI OverlayThread(LPVOID)
{
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"AstralOverlayClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    hwndOverlay = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        wc.lpszClassName,
        L"AstralOverlay",
        WS_POPUP,
        0, 0, width, height,
        nullptr, nullptr,
        wc.hInstance,
        nullptr);

    if (!hwndOverlay)
        return 1;

    // Initialize memory DC and bitmap
    HDC hdcScreen = GetDC(nullptr);
    hMemDC = CreateCompatibleDC(hdcScreen);
    ReleaseDC(nullptr, hdcScreen);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hBitmap = CreateDIBSection(hMemDC, &bmi, DIB_RGB_COLORS, (void**)&pBits, nullptr, 0);
    SelectObject(hMemDC, hBitmap);

    SetWindowPos(hwndOverlay, HWND_TOPMOST, 0, 0, width, height, SWP_SHOWWINDOW);

    lastTick = GetTickCount();

    while (running)
    {
        // Clear to transparent
        ZeroMemory(pBits, width * height * 4);

        RECT rc = { 0, 0, width, height };
        int cx = width / 2;
        int cy = height / 2;

        // Calculate FPS
        DWORD now = GetTickCount();
        frameCount++;
        if (now - lastTick >= 1000)
        {
            currentFPS = frameCount * 1000.0f / (now - lastTick);
            lastTick = now;
            frameCount = 0;
        }

        // Crosshair color
        COLORREF drawColor;
        if (rainbowEnabled)
        {
            static DWORD startTime = GetTickCount();
            float seconds = (GetTickCount() - startTime) / 1000.0f;
            float hue = fmodf(seconds * 0.3f, 1.0f);
            drawColor = HSVtoRGB(hue, 1.0f, 1.0f);
        }
        else
        {
            drawColor = RGB(colorR, colorG, colorB);
        }

        // Draw scope overlay if enabled
        if (scopeOverlayEnabled)
            DrawScopeOverlay(hMemDC, cx, cy, scopeRadius, scopeOffsetX, scopeOffsetY);

        if (crosshairEnabled)
            DrawCrosshair(hMemDC, cx, cy, drawColor, crosshairSize, crosshairGap, crosshairShape);

        if (watermarkEnabled)
            DrawWatermark(hMemDC);

        DrawInfoPanel(hMemDC);

        if (menuOpen)
            DrawMenu(hMemDC);

        // Draw kill effect text
        killEffect.Draw(hMemDC);

        POINT ptWinPos = { 0, 0 };
        SIZE sizeWin = { width, height };
        POINT ptSrc = { 0, 0 };

        BLENDFUNCTION blend = { 0 };
        blend.BlendOp = AC_SRC_OVER;
        blend.BlendFlags = 0;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UpdateLayeredWindow(hwndOverlay, nullptr, &ptWinPos, &sizeWin, hMemDC, &ptSrc, 0, &blend, ULW_ALPHA);

        // Input and message processing
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        ProcessInput();

        Sleep(10);
    }

    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    DestroyWindow(hwndOverlay);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
