#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "renderer.h"

static bool quit = false;

LRESULT CALLBACK WindowProcessMessage(HWND, UINT, WPARAM, LPARAM);
#if RAND_MAX == 32767
#define Rand32() ((rand() << 16) + (rand() << 1) + (rand() & 1))
#else
#define Rand32() rand()
#endif

#define FRAME_RATE 60

static BITMAPINFO frame_bitmap_info;
static HBITMAP frame_bitmap = 0;
static HDC frame_device_context = 0;

struct {
    int width;
    int height;
    uint32_t *pixels;
} frame = {0};

typedef struct Dimensions {
    int x;
    int y;
} Dimensions;

struct {
    int x;
    int y;
    bool left;
    bool right;
} MouseStatus;

struct {
    bool work;
} PanStatus;

Dimensions getRectDimensions(RECT* windowRect) {
    Dimensions result = {
        windowRect->right - windowRect->left,
        windowRect->bottom - windowRect->top
    };
    return result;
}

Dimensions getBorderDimensions(HWND window_handle) {
    RECT windowRect, clientRect;
    GetWindowRect(window_handle, &windowRect);
    GetClientRect(window_handle, &clientRect);
    Dimensions result = {
        windowRect.right - windowRect.left - (clientRect.right - clientRect.left),
        windowRect.bottom - windowRect.top - (clientRect.bottom - clientRect.top)
    };
    return result;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nCmdShow) {
    const wchar_t window_class_name[] = L"My Window Class";
    static WNDCLASS window_class = { 0 };
    window_class.lpfnWndProc = WindowProcessMessage;
    window_class.hInstance = hInstance;
    window_class.lpszClassName = window_class_name;
    RegisterClass(&window_class);

    frame_bitmap_info.bmiHeader.biSize = sizeof(frame_bitmap_info.bmiHeader);
    frame_bitmap_info.bmiHeader.biPlanes = 1;
    frame_bitmap_info.bmiHeader.biBitCount = 32;
    frame_bitmap_info.bmiHeader.biCompression = BI_RGB;
    frame_device_context = CreateCompatibleDC(0);

    static HWND window_handle;
    window_handle = CreateWindow(window_class_name, L"Fractal", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 640, 300, 640, 480, NULL, NULL, hInstance, NULL);
    if(window_handle == NULL) { return -1; }
    
    if (rendererInitialize()) {
        printf("Error initializing renderer\n");
        rendererExit();
        return -1;
    }

    timeBeginPeriod(1);
    LARGE_INTEGER perfFrequency, perfStart, perfNext, perfCurr;
    // How many performance counts are there in a second
    QueryPerformanceFrequency(&perfFrequency);
    QueryPerformanceCounter(&perfStart);
    perfNext = perfStart;

    while (!quit) {
        static MSG message = { 0 };
        while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
            DispatchMessage(&message);
        }

        QueryPerformanceCounter(&perfCurr);
        // For some reason there was a delay of more than 0.5 s, reset next to curr
        if ((perfCurr.QuadPart * 1000 - perfNext.QuadPart * 1000) / perfFrequency.QuadPart > 500) {
            perfNext = perfCurr;
        }

        // Thread wait if more than 3000us
        int64_t micros = (perfNext.QuadPart * 1000000 - perfCurr.QuadPart * 1000000) / perfFrequency.QuadPart;
        if (micros >= 6500) {
            Sleep(5);
            continue;
        } else if (micros >= 3000) {
            Sleep((micros - 1500) / 1000);
            continue;
        }
        // Wait out the rest with a spinlock to avoid sleep inaccuracy
        while (micros > 0) {
            QueryPerformanceCounter(&perfCurr);
            micros = (perfNext.QuadPart * 1000000 - perfCurr.QuadPart * 1000000) / perfFrequency.QuadPart;
        }
        
        QueryPerformanceCounter(&perfCurr);
        micros = (perfCurr.QuadPart * 1000000 - perfStart.QuadPart * 1000000) / perfFrequency.QuadPart;
        perfNext.QuadPart += perfFrequency.QuadPart / FRAME_RATE;

        if (tryRedraw32(frame.pixels, frame.width, frame.height)) {
            InvalidateRect(window_handle, NULL, FALSE);
            UpdateWindow(window_handle);
        }
    }

    timeEndPeriod(1);

    rendererExit();
    return 0;
}


LRESULT CALLBACK WindowProcessMessage(HWND window_handle, UINT message, WPARAM wParam, LPARAM lParam) {
    switch(message) {
        case WM_QUIT:
        case WM_DESTROY: {
            quit = true;
        } break;

        case WM_PAINT: {
            static PAINTSTRUCT paint;
            static HDC device_context;
            device_context = BeginPaint(window_handle, &paint);
            BitBlt(device_context,
                   paint.rcPaint.left, paint.rcPaint.top,
                   paint.rcPaint.right - paint.rcPaint.left, paint.rcPaint.bottom - paint.rcPaint.top,
                   frame_device_context,
                   paint.rcPaint.left, paint.rcPaint.top,
                   SRCCOPY);
            EndPaint(window_handle, &paint);
        } break;

        case WM_WINDOWPOSCHANGING: {
            //((WINDOWPOS*)lParam)->cx = ((WINDOWPOS*)lParam)->cy;
        } break;

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);

            frame_bitmap_info.bmiHeader.biWidth  = width;
            frame_bitmap_info.bmiHeader.biHeight = -height;

            if(frame_bitmap) DeleteObject(frame_bitmap);
            frame_bitmap = CreateDIBSection(NULL, &frame_bitmap_info, DIB_RGB_COLORS, (void**)&frame.pixels, 0, 0);
            SelectObject(frame_device_context, frame_bitmap);

            frame.width =  width;
            frame.height = height;

            resizeFrame(frame.width, frame.height);
        } break;

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
            int newX = LOWORD(lParam);
            int newY = HIWORD(lParam);
            bool newLeft = wParam & MK_LBUTTON ? true : false;
            bool newRight = wParam & MK_RBUTTON ? true : false;
            if (MouseStatus.left == true && newLeft == true && (MouseStatus.x != newX || MouseStatus.y != newY)) {
                // printf("Pan by: %d, %d\n", newX - MouseStatus.x, newY - MouseStatus.y);
                panFrame(newX - MouseStatus.x, newY - MouseStatus.y);
            }
            MouseStatus.x = newX; MouseStatus.y = newY;
            MouseStatus.left = newLeft; MouseStatus.right = newRight;
        } break;

        case WM_MOUSEWHEEL: {
            // printf("scroll %d\n", (int16_t)HIWORD(wParam));
            int newX = LOWORD(lParam);
            int newY = HIWORD(lParam);
            zoomFrame(newX, newY, (int16_t)HIWORD(wParam) < 0 ? 1 : -1);
        } break;

        case WM_MOUSELEAVE: {
            MouseStatus.left = false;
            MouseStatus.right = false;
        }

        default: {
            return DefWindowProc(window_handle, message, wParam, lParam);
        }
    }
    return 0;
}
