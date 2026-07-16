#include "App.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace {
D2D1_COLOR_F color(UINT32 hex, float alpha = 1.f) {
    return D2D1::ColorF((hex >> 16 & 255) / 255.f, (hex >> 8 & 255) / 255.f, (hex & 255) / 255.f, alpha);
}
std::wstring editText(HWND edit) {
    int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length) GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}
std::wstring formatMemory(float mb) {
    int halfGigabytes = std::clamp(static_cast<int>(std::round(mb / 512.f)), 4, 32);
    return std::to_wstring(halfGigabytes / 2) + (halfGigabytes % 2 ? L".5" : L"") + L" GB";
}
}

int App::run(HINSTANCE instance, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    core_.bootstrap();
    ramVisualMb_ = static_cast<float>(core_.settings().ramMb);
    for (size_t i = 0; i < core_.manifest().modules.size() && i < 64; ++i)
        moduleToggleAnim_[i] = core_.moduleEnabled(i) ? 1.f : 0.f;
    firstRun_ = !core_.settings().firstRunComplete || core_.settings().nickname.empty();
    onboardingAnim_ = firstRun_ ? 1.f : 0.f;
    if (core_.settings().nickname.empty()) {
        wchar_t user[256]{}; DWORD size = 256;
        if (GetUserNameW(user, &size)) core_.settings().nickname = user;
        if (core_.settings().nickname.empty()) core_.settings().nickname = L"Player";
    }
    if (!createWindow(instance, show)) { CoUninitialize(); return 1; }
    syncCoreToEdits();
    updateEditVisibility();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    discardGraphics();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

bool App::createWindow(HINSTANCE instance, int show) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NeverloseLoaderWindow";
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = 1080, h = 680;
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.top + (work.bottom - work.top - h) / 2;
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Neverlose Loader",
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    dpi_ = GetDpiForWindow(hwnd_);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    DWORD editStyle = WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | WS_TABSTOP;
    nickEdit_ = CreateWindowExW(0, L"EDIT", L"", editStyle, 0, 0, 100, 30, hwnd_, reinterpret_cast<HMENU>(101), instance, nullptr);
    pathEdit_ = CreateWindowExW(0, L"EDIT", L"", editStyle, 0, 0, 100, 30, hwnd_, reinterpret_cast<HMENU>(102), instance, nullptr);
    setEditFont(nickEdit_); setEditFont(pathEdit_);

    SetTimer(hwnd_, 1, 16, nullptr);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    return true;
}

void App::setEditFont(HWND edit) {
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 10));
}

LRESULT CALLBACK App::wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(l);
        app = static_cast<App*>(create->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handle(msg, w, l) : DefWindowProcW(hwnd, msg, w, l);
}

LRESULT App::handle(UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(l);
            info->ptMinTrackSize = {960, 620};
            return 0;
        }
        case WM_SIZE: resize(LOWORD(l), HIWORD(l)); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            auto rect = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            discardGraphics();
            return 0;
        }
        case WM_PAINT: render(); return 0;
        case WM_TIMER: {
            time_ += .016f;
            transition_ = std::clamp(transition_ + (1.f - transition_) * .14f, 0.f, 1.f);
            introAnim_ = std::clamp(introAnim_ + (1.f - introAnim_) * .10f, 0.f, 1.f);
            versionDropdownAnim_ = std::clamp(versionDropdownAnim_ + ((versionDropdownOpen_ ? 1.f : 0.f) - versionDropdownAnim_) * .18f, 0.f, 1.f);
            moduleScroll_ += (moduleScrollTarget_ - moduleScroll_) * .16f;
            if (std::abs(moduleScrollTarget_ - moduleScroll_) < .05f) moduleScroll_ = moduleScrollTarget_;
            onboardingAnim_ = std::clamp(onboardingAnim_ + ((firstRun_ ? 1.f : 0.f) - onboardingAnim_) * .13f, 0.f, 1.f);
            if (transition_ > .998f) transition_ = 1.f;
            float targetRam = static_cast<float>(core_.settings().ramMb);
            ramVelocity_ += (targetRam - ramVisualMb_) * .045f;
            ramVelocity_ *= ramDragging_ ? .76f : .70f;
            float nextRam = ramVisualMb_ + ramVelocity_;
            if (nextRam <= 2048.f || nextRam >= 16384.f) ramVelocity_ = 0.f;
            ramVisualMb_ = std::clamp(nextRam, 2048.f, 16384.f);
            if (!ramDragging_ && std::abs(targetRam - ramVisualMb_) < .2f && std::abs(ramVelocity_) < .2f) {
                ramVisualMb_ = targetRam; ramVelocity_ = 0.f;
            }
            RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
            return 0;
        }
        case WM_MOUSEMOVE: {
            float x = static_cast<float>(GET_X_LPARAM(l));
            float y = static_cast<float>(GET_Y_LPARAM(l));
            if (ramDragging_) setMemoryFromX(x);
            mouseMove(x, y);
            return 0;
        }
        case WM_LBUTTONUP:
            if (ramDragging_) { ramDragging_ = false; ReleaseCapture(); core_.saveSettings(); }
            return 0;
        case WM_CAPTURECHANGED:
            ramDragging_ = false;
            return 0;
        case WM_MOUSEWHEEL:
            if (page_ == 1 && !busy_) {
                size_t rows = (core_.manifest().modules.size() + 1) / 2;
                float contentHeight = rows ? static_cast<float>(rows) * 94.f - 12.f : 0.f;
                float availableHeight = std::max(0.f, height_ - 266.f);
                float maximum = std::max(0.f, contentHeight - availableHeight);
                moduleScrollTarget_ = std::clamp(moduleScrollTarget_ - static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / 120.f * 78.f, 0.f, maximum);
                return 0;
            }
            return DefWindowProcW(hwnd_, msg, w, l);
        case WM_LBUTTONDOWN: {
            float x = static_cast<float>(GET_X_LPARAM(l));
            float y = static_cast<float>(GET_Y_LPARAM(l));
            bool interactive = false;
            for (const auto& hit : hits_) if (hit.rect.contains(x, y)) { interactive = true; break; }
            if (!interactive && y < 48) {
                ReleaseCapture();
                SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            } else click(x, y);
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                if (versionDropdownOpen_) versionDropdownOpen_ = false;
                else if (busy_) cancel_ = true;
                else DestroyWindow(hwnd_);
            }
            return 0;
        case WM_KILLFOCUS:
            versionDropdownOpen_ = false;
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, RGB(224, 233, 255));
            SetBkColor(dc, RGB(10, 16, 32));
            static HBRUSH background = CreateSolidBrush(RGB(10, 16, 32));
            return reinterpret_cast<LRESULT>(background);
        }
        case WM_APP + 1:
            busy_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            if (busy_) cancel_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, 1);
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(hwnd_, msg, w, l);
    }
}

bool App::createGraphics() {
    if (!d2dFactory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) return false;
    if (!writeFactory_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) return false;
    if (!wicFactory_) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory_.GetAddressOf()));
    if (!target_) {
        RECT rect{}; GetClientRect(hwnd_, &rect);
        if (FAILED(d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rect.right, rect.bottom)), target_.GetAddressOf()))) return false;
        target_->CreateSolidColorBrush(color(0xFFFFFF), brush_.GetAddressOf());
        loadAvatar();
        loadLogo();
    }
    auto makeFont = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
        if (out) return;
        HRESULT hr = writeFactory_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", out.GetAddressOf());
        if (FAILED(hr)) writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", out.GetAddressOf());
    };
    makeFont(32, DWRITE_FONT_WEIGHT_SEMI_BOLD, displayFont_);
    makeFont(19, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFont_);
    makeFont(14, DWRITE_FONT_WEIGHT_NORMAL, bodyFont_);
    makeFont(12, DWRITE_FONT_WEIGHT_NORMAL, smallFont_);
    makeFont(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonFont_);
    return true;
}

void App::discardGraphics() {
    avatarBitmap_.Reset(); logoBitmap_.Reset(); brush_.Reset(); target_.Reset(); displayFont_.Reset(); titleFont_.Reset(); bodyFont_.Reset();
    smallFont_.Reset(); buttonFont_.Reset(); writeFactory_.Reset(); d2dFactory_.Reset();
}

void App::resize(UINT w, UINT h) {
    width_ = static_cast<float>(w); height_ = static_cast<float>(h);
    if (target_) target_->Resize(D2D1::SizeU(w, h));
    updateEditVisibility();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::roundRect(Rect r, float radius, D2D1_COLOR_F fill, float border, D2D1_COLOR_F borderColor) {
    auto rr = D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), radius, radius);
    brush_->SetColor(fill); target_->FillRoundedRectangle(rr, brush_.Get());
    if (border > 0) { brush_->SetColor(borderColor); target_->DrawRoundedRectangle(rr, brush_.Get(), border); }
}

void App::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width) {
    brush_->SetColor(c); target_->DrawLine({x1, y1}, {x2, y2}, brush_.Get(), width);
}

void App::drawText(const std::wstring& text, Rect r, D2D1_COLOR_F c, IDWriteTextFormat* format, DWRITE_TEXT_ALIGNMENT alignment) {
    if (!format) return;
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    brush_->SetColor(c);
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
        D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::drawIcon(Icon icon, Rect r, D2D1_COLOR_F c, float stroke) {
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    brush_->SetColor(c);
    switch (icon) {
        case Icon::Logo:
            line(cx - 10, cy + 8, cx - 10, cy - 8, c, 2.4f); line(cx - 10, cy - 8, cx - 1, cy + 8, c, 2.4f); line(cx - 1, cy + 8, cx - 1, cy - 8, c, 2.4f);
            line(cx + 3, cy - 8, cx + 3, cy + 8, c, 2.4f); line(cx + 3, cy + 8, cx + 11, cy + 8, c, 2.4f);
            break;
        case Icon::Home:
            line(cx - 8, cy, cx, cy - 7, c, stroke); line(cx, cy - 7, cx + 8, cy, c, stroke); line(cx - 6, cy - 1, cx - 6, cy + 7, c, stroke); line(cx - 6, cy + 7, cx + 6, cy + 7, c, stroke); line(cx + 6, cy + 7, cx + 6, cy - 1, c, stroke); break;
        case Icon::Grid:
            for (int yy = 0; yy < 2; ++yy) for (int xx = 0; xx < 2; ++xx) target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8 + xx * 10, cy - 8 + yy * 10, cx - 1 + xx * 10, cy - 1 + yy * 10), 2, 2), brush_.Get(), stroke); break;
        case Icon::Settings:
            target_->DrawEllipse(D2D1::Ellipse({cx, cy}, 4, 4), brush_.Get(), stroke);
            for (int i = 0; i < 8; ++i) { float a = i * 3.1415926f / 4; line(cx + std::cos(a) * 7, cy + std::sin(a) * 7, cx + std::cos(a) * 10, cy + std::sin(a) * 10, c, stroke); } break;
        case Icon::Folder:
            target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 9, cy - 6, cx + 9, cy + 7), 3, 3), brush_.Get(), stroke); line(cx - 8, cy - 6, cx - 2, cy - 6, c, stroke); line(cx - 2, cy - 6, cx + 1, cy - 3, c, stroke); break;
        case Icon::Refresh:
            target_->DrawEllipse(D2D1::Ellipse({cx, cy}, 8, 8), brush_.Get(), stroke); line(cx + 5, cy - 7, cx + 9, cy - 7, c, stroke); line(cx + 9, cy - 7, cx + 9, cy - 3, c, stroke); break;
        case Icon::Play:
            line(cx - 4, cy - 7, cx + 7, cy, c, 2); line(cx + 7, cy, cx - 4, cy + 7, c, 2); line(cx - 4, cy + 7, cx - 4, cy - 7, c, 2); break;
        case Icon::User:
            target_->DrawEllipse(D2D1::Ellipse({cx, cy - 5}, 4, 4), brush_.Get(), stroke); target_->DrawEllipse(D2D1::Ellipse({cx, cy + 9}, 8, 7), brush_.Get(), stroke); break;
        case Icon::Check: line(cx - 7, cy, cx - 2, cy + 5, c, 2); line(cx - 2, cy + 5, cx + 8, cy - 6, c, 2); break;
        case Icon::Download: line(cx, cy - 8, cx, cy + 4, c, stroke); line(cx - 5, cy, cx, cy + 5, c, stroke); line(cx, cy + 5, cx + 5, cy, c, stroke); line(cx - 8, cy + 9, cx + 8, cy + 9, c, stroke); break;
        case Icon::Memory:
            target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 7, cy - 7, cx + 7, cy + 7), 2, 2), brush_.Get(), stroke); target_->DrawRectangle(D2D1::RectF(cx - 3, cy - 3, cx + 3, cy + 3), brush_.Get(), stroke); break;
        case Icon::Chevron: line(cx - 3, cy - 5, cx + 3, cy, c, stroke); line(cx + 3, cy, cx - 3, cy + 5, c, stroke); break;
        case Icon::Close: line(cx - 5, cy - 5, cx + 5, cy + 5, c, stroke); line(cx + 5, cy - 5, cx - 5, cy + 5, c, stroke); break;
        case Icon::Minimize: line(cx - 6, cy, cx + 6, cy, c, stroke); break;
    }
}

void App::addHit(Rect rect, HitType type, size_t index) { hits_.push_back({rect, type, index}); }

void App::renderTitlebar() {
    drawText(L"NEVERLOSE  /  LOADER", {244, 8, 250, 34}, color(0x6980A8), smallFont_.Get());
    Rect minButton{width_ - 88, 8, 34, 32};
    Rect closeButton{width_ - 47, 8, 34, 32};
    bool minHover = minButton.contains(hoverX_, hoverY_);
    bool closeHover = closeButton.contains(hoverX_, hoverY_);
    windowButtonAnim_[0]+=((minHover?1.f:0.f)-windowButtonAnim_[0])*.18f;
    windowButtonAnim_[1]+=((closeHover?1.f:0.f)-windowButtonAnim_[1])*.18f;
    roundRect(minButton, 8, color(0x294A8A, .32f*windowButtonAnim_[0]));
    roundRect(closeButton, 8, color(0xE54864, .76f*windowButtonAnim_[1]));
    drawIcon(Icon::Minimize, minButton, color(0x9AAED0));
    drawIcon(Icon::Close, closeButton, color(0xB9C8E3));
    addHit(minButton, HitType::Minimize); addHit(closeButton, HitType::Close);
}

void App::renderSidebar() {
    Rect sidebar{14, 14, 206, height_ - 28};
    roundRect(sidebar, 22, color(0x080D1A, .98f), 1, color(0x23345A, .72f));

    Rect logoPlate{30, 30, 50, 50};
    roundRect(logoPlate, 15, color(0x102553), .9f, color(0x2E6EFF, .48f));
    drawLogo({34, 34, 42, 42});
    drawText(L"NEVERLOSE", {92, 36, 104, 34}, color(0xF2F6FF), buttonFont_.Get());
    line(30, 94, 202, 94, color(0x253656, .64f), 1.f);

    Icon icons[] = {Icon::Home, Icon::Grid, Icon::Settings};
    const wchar_t* labels[] = {L"Dashboard", L"Components", L"Settings"};
    for (size_t i = 0; i < 3; ++i) {
        Rect item{28, 116 + static_cast<float>(i) * 54, 178, 42};
        bool active = page_ == static_cast<int>(i);
        bool hover = item.contains(hoverX_, hoverY_);
        navAnim_[i] += (((hover || active) ? 1.f : 0.f) - navAnim_[i]) * .16f;
        float n = navAnim_[i];
        if (n > .01f) roundRect(item, 11, active ? color(0x142B59, .92f) : color(0x10203D, .58f*n), 1, active ? color(0x2F6FFF, .38f+.2f*n) : color(0x29436F, .2f*n));
        drawIcon(icons[i], {41, item.y + 9, 24, 24}, active ? color(0x6DA2FF) : color(0x627799), 1.55f);
        drawText(labels[i], {78, item.y, 110, item.h}, active ? color(0xEDF4FF) : color(0x8496B5), bodyFont_.Get());
        addHit(item, HitType::Nav, i);
    }

    Rect account{28, height_ - 82, 178, 54};
    bool accountHover=account.contains(hoverX_,hoverY_);
    avatarHoverAnim_ += ((accountHover?1.f:0.f)-avatarHoverAnim_)*.14f;
    roundRect(account, 14, accountHover?color(0x111E38):color(0x0D162A), 1, color(0x2C4D82, .62f+.24f*avatarHoverAnim_));
    Rect avatar{39, account.y + 9, 36, 36};
    roundRect({avatar.x-2, avatar.y-2, 40, 40}, 20, color(0x173264), 1, color(0x3778F2, .52f));
    drawAvatar(avatar);
    drawText(core_.settings().nickname.empty() ? L"Player" : core_.settings().nickname, {88, account.y + 5, 98, 25}, color(0xDDE8FF), buttonFont_.Get());
    drawText(L"Local profile", {88, account.y + 27, 98, 18}, color(0x58719D), smallFont_.Get());
    addHit(account, HitType::Avatar);
}

void App::renderHome(Rect area) {
    drawText(L"Ready to play", {area.x, area.y, area.w, 42}, color(0xF5F8FF), displayFont_.Get());
    drawText(L"Minecraft 1.21  /  Fabric environment", {area.x, area.y + 42, area.w, 23}, color(0x6F83A8), bodyFont_.Get());

    float top = area.y + 88;
    float rightW = std::max(230.f, area.w * .31f);
    Rect hero{area.x, top, area.w - rightW - 16, 318};
    roundRect(hero, 22, color(0x0A1122), 1, color(0x263A62, .74f));

    D2D1_GRADIENT_STOP heroStops[] = {{0, color(0x1E5BFF, .18f)}, {1, color(0x102A66, 0)}};
    ComPtr<ID2D1GradientStopCollection> heroCollection; ComPtr<ID2D1RadialGradientBrush> heroGlow;
    target_->CreateGradientStopCollection(heroStops, 2, heroCollection.GetAddressOf());
    target_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(hero.x+hero.w*.76f,hero.y+hero.h*.22f),D2D1::Point2F(0,0),260,220), heroCollection.Get(), heroGlow.GetAddressOf());
    if(heroGlow) target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(hero.x,hero.y,hero.x+hero.w,hero.y+hero.h),22,22),heroGlow.Get());

    roundRect({hero.x + 24, hero.y + 24, 58, 58}, 17, color(0x112A60), 1, color(0x3477FF, .5f));
    drawLogo({hero.x + 29, hero.y + 29, 48, 48});
    drawText(L"Neverlose Client", {hero.x + 101, hero.y + 22, hero.w - 130, 32}, color(0xF0F5FF), titleFont_.Get());
    drawText(L"stable release  •  verified files", {hero.x + 101, hero.y + 51, hero.w - 130, 22}, color(0x647DA9), smallFont_.Get());

    drawText(L"SELECTED BUILD", {hero.x + 25, hero.y + 105, 160, 18}, color(0x526A94), smallFont_.Get());
    Rect build{hero.x + 24, hero.y + 130, hero.w - 48, 58};
    bool buildHover=build.contains(hoverX_,hoverY_);
    roundRect(build, 13, buildHover?color(0x11203D):color(0x0E1A33), 1, color(0x315083, .72f+.18f*(buildHover?1.f:versionDropdownAnim_)));
    size_t selectedIndex=core_.selectedVersionIndex();
    std::wstring selectedName=selectedIndex<core_.manifest().versions.size()?core_.manifest().versions[selectedIndex].name:L"Minecraft 1.21";
    drawText(selectedName, {build.x + 18, build.y, build.w - 150, build.h}, color(0xE8F0FF), buttonFont_.Get());
    roundRect({build.x + build.w - 116, build.y + 18, 72, 23}, 11.5f, color(0x14386F), 1, color(0x347DFF, .38f));
    drawText(L"FABRIC", {build.x + build.w - 116, build.y + 18, 72, 23}, color(0x72A8FF), smallFont_.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    float chevron=versionDropdownAnim_;
    float ccx=build.x+build.w-23,ccy=build.y+build.h/2;
    line(ccx-5,ccy-3+6*chevron,ccx,ccy+2-4*chevron,color(0x7F9CC8),1.6f);
    line(ccx,ccy+2-4*chevron,ccx+5,ccy-3+6*chevron,color(0x7F9CC8),1.6f);
    addHit(build,HitType::VersionDropdown);

    Rect launch{hero.x + 24, hero.y + hero.h - 66, hero.w - 48, 42};
    bool launchHover = launch.contains(hoverX_, hoverY_);
    launchAnim_ += (((launchHover && !busy_) ? 1.f : 0.f) - launchAnim_) * .15f;
    float la = launchAnim_;
    roundRect(launch, 12, busy_ ? color(0x13213B) : D2D1::ColorF(.12f+.05f*la,.35f+.09f*la,.94f,1), 1, busy_ ? color(0x2F4772,.5f) : color(0x76A9FF,.34f+.22f*la));
    if (!busy_) {
        float sheen=launch.x+std::fmod(time_*92.f,launch.w+64.f)-32.f;
        line(sheen,launch.y+7,sheen+13,launch.y+launch.h-7,color(0xFFFFFF,.055f+.055f*la),6.f);
        drawIcon(Icon::Play, {launch.x + 18, launch.y + 10 - la, 22, 22}, color(0xF7FAFF), 1.8f);
    } else {
        float cx=launch.x+29,cy=launch.y+21;
        for(int i=0;i<8;++i){float a=time_*3.f+i*.7854f;roundRect({cx+std::cos(a)*7-1,cy+std::sin(a)*7-1,2,2},1,color(0x8CB5FF,(i+1)/8.f));}
    }
    drawText(busy_ ? L"Cancel installation" : L"Launch client", {launch.x+42,launch.y,launch.w-58,launch.h}, color(busy_?0x8294B3:0xFFFFFF), buttonFont_.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    addHit(launch,HitType::Launch);

    Rect status{hero.x + hero.w + 16, top, rightW, 148};
    roundRect(status, 20, color(0x0A1122), 1, color(0x263A62,.72f));
    roundRect({status.x+20,status.y+21,10,10},5,core_.manifest().maintenance?color(0xFF5973):color(0x4C8CFF));
    drawText(core_.manifest().maintenance?L"Maintenance":L"Service online", {status.x+42,status.y+12,status.w-58,29}, color(0xE8F0FF),buttonFont_.Get());
    std::wstring state; if(busy_){std::lock_guard lock(statusMutex_);state=asyncStatus_;}else state=core_.status();
    drawText(state,{status.x+20,status.y+52,status.w-40,55},color(0x7185A9),smallFont_.Get());
    drawText(L"CHANNEL  /  STABLE",{status.x+20,status.y+116,status.w-40,18},color(0x405A87),smallFont_.Get());

    Rect memoryCard{status.x,top+164,rightW,154};
    roundRect(memoryCard,20,color(0x0A1122),1,color(0x263A62,.72f));
    roundRect({memoryCard.x+18,memoryCard.y+18,36,36},11,color(0x102653));
    drawIcon(Icon::Memory,{memoryCard.x+24,memoryCard.y+24,24,24},color(0x70A3FF));
    drawText(L"Memory",{memoryCard.x+67,memoryCard.y+13,memoryCard.w-85,28},color(0xE5EEFF),buttonFont_.Get());
    drawText(formatMemory(ramVisualMb_)+L" allocated",{memoryCard.x+20,memoryCard.y+61,memoryCard.w-40,21},color(0x6F84A9),smallFont_.Get());
    float memory=std::clamp((ramVisualMb_-2048.f)/14336.f,0.f,1.f);
    roundRect({memoryCard.x+20,memoryCard.y+102,memoryCard.w-40,6},3,color(0x1A2945));
    roundRect({memoryCard.x+20,memoryCard.y+102,(memoryCard.w-40)*memory,6},3,color(0x3478F6));
    drawText(L"2 GB",{memoryCard.x+20,memoryCard.y+116,50,18},color(0x40597F),smallFont_.Get());
    drawText(L"16 GB",{memoryCard.x+memoryCard.w-70,memoryCard.y+116,50,18},color(0x40597F),smallFont_.Get(),DWRITE_TEXT_ALIGNMENT_TRAILING);

    if(busy_){
        Rect progress{area.x,top+336,area.w,56};roundRect(progress,15,color(0x0A1122),1,color(0x263A62,.65f));
        drawText(L"Preparing client files",{progress.x+18,progress.y+5,230,24},color(0xC9D8F3),buttonFont_.Get());
        roundRect({progress.x+18,progress.y+38,progress.w-36,5},2.5f,color(0x182843));
        roundRect({progress.x+18,progress.y+38,(progress.w-36)*progress_.load(),5},2.5f,color(0x397FFF));
    }

    if(versionDropdownAnim_>.01f){
        size_t count=std::min<size_t>(8,core_.manifest().versions.size());
        float panelY=build.y+build.h+7,panelH=(12.f+count*44.f)*versionDropdownAnim_;
        addHit({0,48,width_,height_-48},HitType::DismissDropdown);
        addHit(build,HitType::VersionDropdown);
        ComPtr<ID2D1Layer> dropdownLayer;target_->CreateLayer(dropdownLayer.GetAddressOf());
        auto dropdownParams=D2D1::LayerParameters(D2D1::RectF(build.x,panelY,build.x+build.w,panelY+panelH));target_->PushLayer(dropdownParams,dropdownLayer.Get());
        roundRect({build.x,panelY,build.w,12.f+count*44.f},13,color(0x091326,.995f),1,color(0x315083,.92f));
        for(size_t i=0;i<count;++i){
            const auto& version=core_.manifest().versions[i];Rect row{build.x+6,panelY+6+i*44.f,build.w-12,38};
            bool hover=row.contains(hoverX_,hoverY_);versionItemAnim_[i]=std::clamp(versionItemAnim_[i]+((hover?1.f:0.f)-versionItemAnim_[i])*.16f,0.f,1.f);
            bool selected=i==selectedIndex;
            if(hover||selected)roundRect(row,9,selected?color(0x17356E,.92f):color(0x122445,.72f*versionItemAnim_[i]),1,selected?color(0x3F83FF,.48f):color(0x345686,.28f*versionItemAnim_[i]));
            drawText(version.name,{row.x+13,row.y,row.w-115,row.h},version.available?color(0xE4EDFF):color(0x657793),buttonFont_.Get());
            std::wstring badge=version.available?(selected?L"SELECTED":L"READY"):(version.badge.empty()?L"SOON":version.badge);
            roundRect({row.x+row.w-91,row.y+8,77,22},11,version.available?color(0x14386F):color(0x191F2D),1,version.available?color(0x347DFF,.34f):color(0x4D5668,.45f));
            drawText(badge,{row.x+row.w-91,row.y+8,77,22},version.available?color(0x72A8FF):color(0x7B8494),smallFont_.Get(),DWRITE_TEXT_ALIGNMENT_CENTER);
            if(versionDropdownAnim_>.82f)addHit(row,HitType::VersionItem,i);
        }
        target_->PopLayer();
    }
}

void App::renderModules(Rect area) {
    drawText(L"Components", {area.x, area.y, area.w, 42}, color(0xF5F8FF), displayFont_.Get());
    drawText(L"Server-synced Fabric components for the selected build.", {area.x, area.y + 42, area.w, 23}, color(0x6F83A8), bodyFont_.Get());
    drawText(L"QUICK PRESET", {area.x, area.y + 88, 150, 18}, color(0x526A94), smallFont_.Get());
    roundRect({area.x+area.w-142,area.y+84,142,24},12,color(0x102B5D),1,color(0x347BFF,.35f));
    drawText(L"MODRINTH  •  LIVE",{area.x+area.w-142,area.y+84,142,24},color(0x78A8FF),smallFont_.Get(),DWRITE_TEXT_ALIGNMENT_CENTER);
    float presetW=(area.w-36)/4.f;
    for(size_t i=0;i<core_.manifest().presets.size()&&i<4;++i){
        Rect preset{area.x+i*(presetW+12),area.y+114,presetW,40};
        bool active=i==core_.selectedPresetIndex(),hover=preset.contains(hoverX_,hoverY_);
        presetAnim_[i]+=(((active||hover)?1.f:0.f)-presetAnim_[i])*.16f;float a=presetAnim_[i];
        roundRect(preset,11,active?color(0x142E61):color(0x0A1326),1,active?color(0x397DFF,.48f+.24f*a):color(0x263A60,.55f+.24f*a));
        if(hover&&!active)roundRect({preset.x+12,preset.y+preset.h-3,preset.w-24,2},1,color(0x4A88FF,.24f*a));
        drawText(core_.manifest().presets[i].name,preset,active?color(0x8DB7FF):color(0x8293B1),buttonFont_.Get(),DWRITE_TEXT_ALIGNMENT_CENTER);
        addHit(preset,HitType::Preset,i);
    }
    float listTop=area.y+172, listBottom=height_-28;
    size_t rowCount=(core_.manifest().modules.size()+1)/2;
    float contentHeight=rowCount?static_cast<float>(rowCount)*94.f-12.f:0.f;
    float maximumScroll=std::max(0.f,contentHeight-(listBottom-(area.y+178)));
    moduleScrollTarget_=std::clamp(moduleScrollTarget_,0.f,maximumScroll);
    moduleScroll_=std::clamp(moduleScroll_,0.f,maximumScroll);
    float startY=area.y+178-moduleScroll_;
    ComPtr<ID2D1Layer> listLayer;target_->CreateLayer(listLayer.GetAddressOf());
    auto listParams=D2D1::LayerParameters(D2D1::RectF(area.x,listTop,area.x+area.w,listBottom));target_->PushLayer(listParams,listLayer.Get());
    for(size_t i=0;i<core_.manifest().modules.size();++i){
        int col=static_cast<int>(i%2),row=static_cast<int>(i/2);
        Rect card{area.x+col*(area.w/2+7),startY+row*94,area.w/2-7,82};
        bool active=core_.moduleEnabled(i),hover=card.contains(hoverX_,hoverY_);
        bool visible=card.y+card.h>=listTop&&card.y<=listBottom;
        if(i<64)moduleAnim_[i]+=(((hover&&visible)?1.f:0.f)-moduleAnim_[i])*.15f;float a=i<64?moduleAnim_[i]:0.f;
        if(i<64)moduleToggleAnim_[i]+=(((active?1.f:0.f)-moduleToggleAnim_[i])*.18f);
        float toggleA=i<64?moduleToggleAnim_[i]:(active?1.f:0.f);
        roundRect(card,16,hover?color(0x0E1A32):color(0x0A1122),1,active?color(0x377BFA,.42f+.12f*a):color(0x263A62,.58f+.18f*a));
        if(hover)roundRect({card.x+1,card.y+card.h-3,card.w-2,2},1,color(0x3A7DFF,.16f*a));
        float iconLift=a*1.5f;
        roundRect({card.x+16,card.y+14-iconLift,38,38},11,active?color(0x14336D):color(0x111D34),1,active?color(0x3E7EF5,.28f):color(0x233A60,.5f));
        drawIcon(active?Icon::Check:Icon::Grid,{card.x+23,card.y+21-iconLift,24,24},active?color(0x75A7FF):color(0x5B7095),1.6f);
        drawText(core_.manifest().modules[i].name,{card.x+68,card.y+8,card.w-138,26},color(0xE8F0FF),buttonFont_.Get());
        drawText(core_.manifest().modules[i].description,{card.x+68,card.y+34,card.w-138,34},color(0x667A9E),smallFont_.Get());
        Rect toggle{card.x+card.w-55,card.y+20,36,20};
        D2D1_COLOR_F toggleColor=D2D1::ColorF(.13f+.08f*toggleA,.20f+.26f*toggleA,.31f+.61f*toggleA,1.f);
        roundRect(toggle,10,toggleColor,1,color(0x5A8DF2,.20f+.28f*toggleA));
        roundRect({toggle.x+2+17*toggleA,toggle.y+2,16,16},8,color(0xEEF4FF),1,color(0xFFFFFF,.25f));
        if(visible)addHit(card,HitType::Module,i);
    }
    target_->PopLayer();
    if(maximumScroll>0){
        float railH=listBottom-listTop;float thumbH=std::max(42.f,railH*railH/(railH+maximumScroll));
        float thumbY=listTop+(railH-thumbH)*(moduleScroll_/maximumScroll);
        roundRect({area.x+area.w-3,thumbY,3,thumbH},1.5f,color(0x4A83ED,.46f));
    }
}

void App::renderSettings(Rect area) {
    drawText(L"Settings", {area.x, area.y, area.w, 42}, color(0xF5F8FF), displayFont_.Get());
    drawText(L"Profile, installation source and runtime limits.", {area.x, area.y + 42, area.w, 23}, color(0x6F83A8), bodyFont_.Get());
    Rect panel{area.x,area.y+88,area.w,382};roundRect(panel,20,color(0x0A1122),1,color(0x263A62,.72f));
    drawText(L"PROFILE",{panel.x+24,panel.y+20,160,18},color(0x526A94),smallFont_.Get());
    drawText(L"Minecraft nickname",{panel.x+24,panel.y+54,175,24},color(0xA9B9D3),bodyFont_.Get());
    roundRect({panel.x+210,panel.y+48,panel.w-234,38},10,color(0x0A1020),1,color(0x2B416B,.72f));
    line(panel.x+24,panel.y+105,panel.x+panel.w-24,panel.y+105,color(0x223452,.65f),1);
    drawText(L"INSTALLATION",{panel.x+24,panel.y+122,160,18},color(0x526A94),smallFont_.Get());
    drawText(L"Game directory",{panel.x+24,panel.y+157,175,24},color(0xA9B9D3),bodyFont_.Get());
    roundRect({panel.x+210,panel.y+151,panel.w-286,38},10,color(0x0A1020),1,color(0x2B416B,.72f));
    Rect browse{panel.x+panel.w-60,panel.y+151,36,38};bool browseHover=browse.contains(hoverX_,hoverY_);actionAnim_[0]+=((browseHover?1.f:0.f)-actionAnim_[0])*.17f;roundRect(browse,10,browseHover?color(0x17376E):color(0x101C34),1,color(0x3C6DB5,.55f+.3f*actionAnim_[0]));drawIcon(Icon::Folder,{browse.x,browse.y-actionAnim_[0],browse.w,browse.h},color(0x76A6F7));addHit(browse,HitType::Browse);
    drawText(L"CONFIGURATION",{panel.x+24,panel.y+207,175,18},color(0x526A94),smallFont_.Get());
    roundRect({panel.x+210,panel.y+197,panel.w-234,40},10,color(0x0A1020),1,color(0x2B416B,.72f));
    roundRect({panel.x+224,panel.y+208,18,18},9,color(0x13316A),1,color(0x4C8DFF,.54f));
    drawIcon(Icon::Check,{panel.x+224,panel.y+208,18,18},color(0x83B2FF),1.3f);
    drawText(L"Official GitHub configuration",{panel.x+252,panel.y+199,panel.w-276,36},color(0xA9C4F5),smallFont_.Get());
    line(panel.x+24,panel.y+258,panel.x+panel.w-24,panel.y+258,color(0x223452,.65f),1);
    drawText(L"RUNTIME MEMORY",{panel.x+24,panel.y+275,180,18},color(0x526A94),smallFont_.Get());
    drawText(L"Memory allocation",{panel.x+24,panel.y+310,175,24},color(0xA9B9D3),bodyFont_.Get());
    drawText(formatMemory(ramVisualMb_),{panel.x+panel.w-95,panel.y+310,70,24},color(0x72A5FF),buttonFont_.Get(),DWRITE_TEXT_ALIGNMENT_TRAILING);
    Rect track{panel.x+210,panel.y+344,panel.w-234,6};float memory=std::clamp((ramVisualMb_-2048.f)/14336.f,0.f,1.f);
    roundRect(track,3,color(0x1A2945));roundRect({track.x,track.y,track.w*memory,6},3,color(0x357AF6));float thumb=track.x+track.w*memory;
    if(ramDragging_)roundRect({thumb-13,track.y-11,26,26},13,color(0x347BFF,.13f));
    roundRect({thumb-(ramDragging_?8.f:7.f),track.y-(ramDragging_?5.f:4.f),ramDragging_?16.f:14.f,ramDragging_?16.f:14.f},8,color(0xF1F6FF),1,color(0x4A89FF,.7f));
    addHit({track.x-8,track.y-12,track.w+16,31},HitType::Ram);
}

void App::drawLogo(Rect rect, float opacity) {
    if (!logoBitmap_) {
        drawIcon(Icon::Logo, rect, color(0x77A9FF, opacity), 2.f);
        return;
    }
    auto size=logoBitmap_->GetSize();
    target_->DrawBitmap(logoBitmap_.Get(),D2D1::RectF(rect.x,rect.y,rect.x+rect.w,rect.y+rect.h),opacity,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,D2D1::RectF(0,0,size.width,size.height));
}

void App::loadLogo() {
    logoBitmap_.Reset();
    if(!target_||!wicFactory_)return;
    HMODULE module=GetModuleHandleW(nullptr);
    HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(102),RT_RCDATA);
    if(!resource)return;
    HGLOBAL loaded=LoadResource(module,resource);if(!loaded)return;
    auto* bytes=static_cast<BYTE*>(LockResource(loaded));DWORD length=SizeofResource(module,resource);
    if(!bytes||!length)return;
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
    if(FAILED(wicFactory_->CreateStream(stream.GetAddressOf())))return;
    if(FAILED(stream->InitializeFromMemory(bytes,length)))return;
    if(FAILED(wicFactory_->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.GetAddressOf())))return;
    if(FAILED(decoder->GetFrame(0,frame.GetAddressOf())))return;
    if(FAILED(wicFactory_->CreateFormatConverter(converter.GetAddressOf())))return;
    if(FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeMedianCut)))return;
    target_->CreateBitmapFromWicBitmap(converter.Get(),nullptr,logoBitmap_.GetAddressOf());
}

void App::drawAvatar(Rect rect) {
    if (!avatarBitmap_) {
        roundRect(rect, rect.w / 2, color(0x151C20));
        drawIcon(Icon::User, rect, color(0x88949B), 1.4f);
        return;
    }
    ComPtr<ID2D1EllipseGeometry> mask;
    d2dFactory_->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(rect.x + rect.w/2, rect.y + rect.h/2), rect.w/2, rect.h/2), mask.GetAddressOf());
    ComPtr<ID2D1Layer> layer; target_->CreateLayer(layer.GetAddressOf());
    auto params = D2D1::LayerParameters(D2D1::RectF(rect.x, rect.y, rect.x+rect.w, rect.y+rect.h), mask.Get());
    target_->PushLayer(params, layer.Get());
    auto size = avatarBitmap_->GetSize();
    float side = std::min(size.width, size.height);
    float sx = (size.width - side)/2, sy = (size.height - side)/2;
    target_->DrawBitmap(avatarBitmap_.Get(), D2D1::RectF(rect.x, rect.y, rect.x+rect.w, rect.y+rect.h), 1.f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, D2D1::RectF(sx, sy, sx+side, sy+side));
    target_->PopLayer();
}

void App::loadAvatar() {
    avatarBitmap_.Reset();
    if (!target_ || !wicFactory_ || core_.settings().avatarPath.empty()) return;
    ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(core_.settings().avatarPath.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) return;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.GetAddressOf()))) return;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
        WICBitmapPaletteTypeMedianCut))) return;
    target_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, avatarBitmap_.GetAddressOf());
}

void App::chooseAvatar() {
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) return;
    COMDLG_FILTERSPEC filters[] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters); dialog->SetTitle(L"Choose profile avatar");
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        ComPtr<IShellItem> item; if (FAILED(dialog->GetResult(item.GetAddressOf()))) return;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            core_.settings().avatarPath = path; CoTaskMemFree(path); core_.saveSettings(); loadAvatar(); InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

void App::renderOnboarding() {
    float alpha=std::clamp(onboardingAnim_,0.f,1.f);
    brush_->SetColor(color(0x030611,.9f*alpha));target_->FillRectangle(D2D1::RectF(0,0,width_,height_),brush_.Get());
    float cardW=430,cardH=400,x=width_/2-cardW/2,y=height_/2-cardH/2+(1-alpha)*14;
    Rect card{x,y,cardW,cardH};roundRect(card,24,color(0x091224,.99f),1,color(0x2C4472,.8f));
    roundRect({x+cardW/2-36,y+25,72,72},20,color(0x102B61),1,color(0x347BFF,.52f));drawLogo({x+cardW/2-30,y+31,60,60});
    drawText(L"Welcome to Neverlose Loader",{x+30,y+111,cardW-60,34},color(0xF2F6FF),titleFont_.Get(),DWRITE_TEXT_ALIGNMENT_CENTER);
    drawText(L"Create a local Minecraft profile",{x+30,y+145,cardW-60,22},color(0x7185A8),bodyFont_.Get(),DWRITE_TEXT_ALIGNMENT_CENTER);
    Rect avatar{x+cardW/2-38,y+181,76,76};roundRect({avatar.x-4,avatar.y-4,84,84},42,color(0x102A5A),1,color(0x347BFF,.45f));drawAvatar(avatar);
    Rect choose{x+cardW/2+55,y+202,92,34};roundRect(choose,10,choose.contains(hoverX_,hoverY_)?color(0x173B78):color(0x111D35),1,color(0x2D4775,.8f));drawText(L"Avatar",choose,color(0xA9BCE0),buttonFont_.Get(),DWRITE_TEXT_ALIGNMENT_CENTER);addHit(choose,HitType::Avatar);
    drawText(L"MINECRAFT NICKNAME",{x+68,y+276,294,18},color(0x526A94),smallFont_.Get());roundRect({x+68,y+300,294,40},11,color(0x0A1020),1,color(0x2B416B,.8f));
    Rect next{x+68,y+354,294,40};bool hover=next.contains(hoverX_,hoverY_);roundRect(next,11,hover?color(0x4386FF):color(0x2E6FEB),1,color(0x7AABFF,.35f));drawText(L"Continue",next,color(0xFFFFFF),buttonFont_.Get(),DWRITE_TEXT_ALIGNMENT_CENTER);addHit(next,HitType::Continue);
}

void App::render() {
    PAINTSTRUCT paint{};BeginPaint(hwnd_,&paint);if(!createGraphics()){EndPaint(hwnd_,&paint);return;}hits_.clear();target_->BeginDraw();target_->Clear(color(0x050914));
    D2D1_GRADIENT_STOP stops[]={{0,color(0x175DE8,.10f)},{1,color(0x071227,0)}};ComPtr<ID2D1GradientStopCollection> collection;ComPtr<ID2D1RadialGradientBrush> glow;
    target_->CreateGradientStopCollection(stops,2,collection.GetAddressOf());float gx=width_*.72f+std::sin(time_*.22f)*38.f;
    target_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(gx,-80),D2D1::Point2F(0,0),520,420),collection.Get(),glow.GetAddressOf());if(glow)target_->FillRectangle(D2D1::RectF(0,0,width_,height_),glow.Get());
    ComPtr<ID2D1RadialGradientBrush> lowerGlow;float gy=height_+90+std::cos(time_*.18f)*28.f;
    target_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(width_*.35f,gy),D2D1::Point2F(0,0),430,260),collection.Get(),lowerGlow.GetAddressOf());if(lowerGlow)target_->FillRectangle(D2D1::RectF(0,0,width_,height_),lowerGlow.Get());
    renderSidebar();renderTitlebar();Rect area{246,60,width_-276,height_-88};
    ComPtr<ID2D1Layer> layer;target_->CreateLayer(layer.GetAddressOf());float pageAlpha=(page_==2?1.f:transition_*transition_*(3.f-2.f*transition_))*introAnim_;
    auto params=D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),pageAlpha);target_->PushLayer(params,layer.Get());
    if(page_==0)renderHome(area);else if(page_==1)renderModules(area);else renderSettings(area);target_->PopLayer();if(onboardingAnim_>.01f)renderOnboarding();
    HRESULT result=target_->EndDraw();if(result==D2DERR_RECREATE_TARGET){avatarBitmap_.Reset();logoBitmap_.Reset();brush_.Reset();target_.Reset();}EndPaint(hwnd_,&paint);
}

void App::mouseMove(float x, float y) {
    hoverX_ = x; hoverY_ = y;
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
}

void App::click(float x, float y) {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        const auto& hit = *it;
        if (!hit.rect.contains(x, y)) continue;
        if (firstRun_ && hit.type != HitType::Continue && hit.type != HitType::Avatar && hit.type != HitType::Close) return;
        if (busy_ && hit.type != HitType::Launch && hit.type != HitType::Close && hit.type != HitType::Minimize) return;
        if (transition_ < .72f && hit.type != HitType::Nav && hit.type != HitType::Close && hit.type != HitType::Minimize) return;
        switch (hit.type) {
            case HitType::Close: DestroyWindow(hwnd_); break;
            case HitType::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
            case HitType::Nav: versionDropdownOpen_=false; page_ = static_cast<int>(hit.index); transition_ = 0; updateEditVisibility(); break;
            case HitType::Browse: browseFolder(); break;
            case HitType::Avatar: chooseAvatar(); break;
            case HitType::Continue:
                syncEditsToCore();
                if (!core_.settings().nickname.empty()) { core_.settings().firstRunComplete = true; core_.saveSettings(); firstRun_ = false; updateEditVisibility(); }
                break;
            case HitType::Preset: core_.selectPreset(hit.index); break;
            case HitType::Module: core_.toggleModule(hit.index); break;
            case HitType::VersionDropdown:
                versionDropdownOpen_=!versionDropdownOpen_;
                break;
            case HitType::VersionItem:
                if(hit.index<core_.manifest().versions.size()&&core_.manifest().versions[hit.index].available){core_.selectVersion(hit.index);versionDropdownOpen_=false;}
                else if(hit.index<core_.manifest().versions.size())core_.setStatus(core_.manifest().versions[hit.index].name+L" is coming soon");
                break;
            case HitType::DismissDropdown:
                versionDropdownOpen_=false;
                break;
            case HitType::Ram:
                ramDragging_ = true;
                SetCapture(hwnd_);
                setMemoryFromX(x);
                break;
            case HitType::Launch: versionDropdownOpen_=false; if (busy_) cancel_ = true; else beginLaunch(); break;
            default: break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}

void App::setMemoryFromX(float x) {
    const float trackX = 456.f;
    const float trackW = std::max(1.f, width_ - 510.f);
    float t = std::clamp((x - trackX) / trackW, 0.f, 1.f);
    core_.settings().ramMb = 2048 + static_cast<int>(std::round(t * 28.f)) * 512;
}

void App::syncEditsToCore() {
    core_.settings().nickname = editText(nickEdit_);
    core_.settings().installDir = editText(pathEdit_);
}
void App::syncCoreToEdits() {
    SetWindowTextW(nickEdit_, core_.settings().nickname.c_str());
    SetWindowTextW(pathEdit_, core_.settings().installDir.c_str());
}

void App::updateEditVisibility() {
    bool settings = page_ == 2 && !firstRun_;
    ShowWindow(nickEdit_, (settings || firstRun_) ? SW_SHOW : SW_HIDE);
    ShowWindow(pathEdit_, settings ? SW_SHOW : SW_HIDE);
    if (firstRun_) {
        SetWindowPos(nickEdit_, HWND_TOP, static_cast<int>(width_/2 - 147), static_cast<int>(height_/2 + 103), 294, 32, SWP_SHOWWINDOW);
        return;
    }
    if (!settings) return;
    float panelX = 246, panelY = 148, panelW = width_ - 276;
    SetWindowPos(nickEdit_, nullptr, static_cast<int>(panelX + 220), static_cast<int>(panelY + 53), static_cast<int>(panelW - 254), 28, SWP_NOZORDER);
    SetWindowPos(pathEdit_, nullptr, static_cast<int>(panelX + 220), static_cast<int>(panelY + 156), static_cast<int>(panelW - 310), 28, SWP_NOZORDER);
}

void App::browseFolder() {
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) return;
    DWORD options = 0; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(dialog->GetResult(item.GetAddressOf()))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { SetWindowTextW(pathEdit_, path); CoTaskMemFree(path); }
        }
    }
}

void App::beginLaunch() {
    syncEditsToCore();
    if (worker_.joinable()) worker_.join();
    cancel_ = false; busy_ = true; progress_ = 0;
    { std::lock_guard lock(statusMutex_); asyncStatus_ = L"Preparing files"; }
    worker_ = std::thread([this] {
        bool ok = core_.launch(cancel_, [this](size_t current, size_t total, const std::wstring& file) {
            progress_ = total ? static_cast<float>(current) / static_cast<float>(total) : 0;
            { std::lock_guard lock(statusMutex_); asyncStatus_ = L"Downloading " + file; }
            PostMessageW(hwnd_, WM_NULL, 0, 0);
        });
        { std::lock_guard lock(statusMutex_); asyncStatus_ = core_.status(); }
        PostMessageW(hwnd_, WM_APP + 1, ok ? 1 : 0, 0);
    });
}
