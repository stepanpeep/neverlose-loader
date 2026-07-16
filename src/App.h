#pragma once
#include "LauncherCore.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class App {
public:
    int run(HINSTANCE instance, int show);

private:
    struct Rect {
        float x, y, w, h;
        bool contains(float px, float py) const { return px >= x && py >= y && px <= x + w && py <= y + h; }
    };

    enum class Icon { Logo, Home, Grid, Settings, Folder, Refresh, Play, User, Check, Download, Memory, Chevron, Close, Minimize };
    enum class HitType { None, Nav, Browse, Avatar, Continue, Refresh, Launch, Preset, Module, Ram, Close, Minimize };
    struct Hit { Rect rect; HitType type; size_t index = 0; };

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    bool createWindow(HINSTANCE, int);
    bool createGraphics();
    void discardGraphics();
    void resize(UINT, UINT);
    void render();
    void renderSidebar();
    void renderHome(Rect area);
    void renderModules(Rect area);
    void renderSettings(Rect area);
    void renderOnboarding();
    void renderTitlebar();
    void drawText(const std::wstring&, Rect, D2D1_COLOR_F, IDWriteTextFormat*, DWRITE_TEXT_ALIGNMENT = DWRITE_TEXT_ALIGNMENT_LEADING);
    void roundRect(Rect, float, D2D1_COLOR_F, float border = 0, D2D1_COLOR_F borderColor = {});
    void line(float, float, float, float, D2D1_COLOR_F, float = 1.5f);
    void drawIcon(Icon, Rect, D2D1_COLOR_F, float = 1.5f);
    void drawLogo(Rect, float opacity = 1.f);
    void drawAvatar(Rect);
    void addHit(Rect, HitType, size_t = 0);
    void click(float, float);
    void mouseMove(float, float);
    void syncEditsToCore();
    void syncCoreToEdits();
    void browseFolder();
    void chooseAvatar();
    void loadAvatar();
    void loadLogo();
    void beginLaunch();
    void setEditFont(HWND);
    void updateEditVisibility();
    void setMemoryFromX(float x);

    HWND hwnd_{};
    HWND nickEdit_{};
    HWND pathEdit_{};
    HWND manifestEdit_{};
    UINT dpi_ = 96;
    float width_ = 1080;
    float height_ = 680;
    float hoverX_ = -1;
    float hoverY_ = -1;
    float transition_ = 1;
    float time_ = 0;
    float onboardingAnim_ = 0;
    float navAnim_[3]{};
    float presetAnim_[4]{};
    float moduleAnim_[64]{};
    float moduleToggleAnim_[64]{};
    float actionAnim_[2]{};
    float windowButtonAnim_[2]{};
    float avatarHoverAnim_ = 0;
    float introAnim_ = 0;
    float moduleScroll_ = 0;
    float moduleScrollTarget_ = 0;
    float launchAnim_ = 0;
    float ramVisualMb_ = 4096.f;
    float ramVelocity_ = 0.f;
    int page_ = 0;
    bool firstRun_ = true;
    bool ramDragging_ = false;

    LauncherCore core_;
    std::vector<Hit> hits_;
    std::atomic_bool cancel_{false};
    std::atomic_bool busy_{false};
    std::atomic<float> progress_{0};
    std::thread worker_;
    std::mutex statusMutex_;
    std::wstring asyncStatus_;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> avatarBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> logoBitmap_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> displayFont_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFont_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFont_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> smallFont_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> buttonFont_;
};
