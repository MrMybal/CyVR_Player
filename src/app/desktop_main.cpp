#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include "cyvr/gpu.hpp"
#include "cyvr/player/application.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <windows.h>

namespace {
using namespace cyvr;
using namespace cyvr::player;
constexpr int height = 1120;
struct Desktop {
    VulkanContext gpu;
    VulkanRenderer renderer{gpu};
    Target target;
    UiModel ui;
    std::unique_ptr<PlayerApplication> app;
    DrawList main, dock;
    std::vector<uint8_t> pixels;
    HWND window{};
    bool pressed{};
    bool consumeCharacter{};
    uint64_t revision{};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Desktop() {
        app.reset();
        if (gpu.device)
            vkDeviceWaitIdle(gpu.device);
        if (target.image)
            renderer.destroyTarget(target);
    }
    void initialize(const std::filesystem::path &assets, const std::string &root) {
        gpu.createStandalone();
        renderer.initialize(VK_FORMAT_R8G8B8A8_SRGB, [&](const std::string &name) {
            std::ifstream file(assets / name, std::ios::binary);
            if (!file)
                throw std::runtime_error("Missing asset: " + name);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
        });
        target = renderer.createTarget(1440, height);
        ui.viewportSize = {1440, float(height)};
        Platform platform;
        platform.root = std::filesystem::absolute(std::filesystem::u8path(root)).u8string();
        app = std::make_unique<PlayerApplication>(ui, std::move(platform));
    }
    void tick() {
        ui.tick(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        app->tick();
        std::vector<uint8_t> thumbnails;if(app->take_thumbnails(thumbnails))renderer.uploadImages(160,630,thumbnails);
        if (revision != ui.revision)
            InvalidateRect(window, nullptr, FALSE);
    }
    void draw() {
        if (revision == ui.revision)
            return;
        ui.drawMain(main);
        if (ui.debugVisible && !ui.interfaceHidden) {
            DrawList hud;
            ui.drawDebug(hud);
            for(auto vertex:hud.vertices){vertex.x=1020+vertex.x*.62f;vertex.y=170+vertex.y*.62f;main.vertices.push_back(vertex);}
        }
        if (ui.windowState != WindowState::Open)
            main.clear();
        ui.drawDock(dock);
        for (auto v : dock.vertices) {
            v.x += dockOriginX;
            v.y += dockOriginY;
            main.vertices.push_back(v);
        }
        renderer.draw(target, main, nullptr, false, ui.interfaceHidden ? Vec2{1440, 990} : Vec2{});
        pixels = renderer.readback(target);
        for (size_t i = 0; i < pixels.size(); i += 4)
            std::swap(pixels[i], pixels[i + 2]);
        revision = ui.revision;
    }
    Vec2 point(LPARAM value) {
        RECT r{};
        GetClientRect(window, &r);
        float scale = std::max(.01f, std::min(r.right / 1440.f, r.bottom / float(height)));
        return {(float(short(LOWORD(value))) - (r.right - 1440 * scale) * .5f) / scale,
                (float(short(HIWORD(value))) - (r.bottom - height * scale) * .5f) / scale};
    }
};
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *d = reinterpret_cast<Desktop *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        d = static_cast<Desktop *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        d->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
    }
    if (!d)
        return DefWindowProcW(window, message, w, l);
    try {
        switch (message) {
        case WM_KEYDOWN:
            if ((l & (LPARAM(1) << 30)) && (d->ui.interfaceHidden || d->consumeCharacter)) return 0;
            if (d->app->show_interface()) { d->consumeCharacter = true; return 0; }
            if(w==VK_LEFT||w==VK_RIGHT){d->app->video_input(w==VK_LEFT?-10.f:10.f,0);return 0;}
            if(w==VK_UP||w==VK_DOWN){d->app->video_input(0,w==VK_UP?.1f:-.1f);return 0;}
            if (w == VK_F11) { d->app->action(1024); return 0; }
            return DefWindowProcW(window, message, w, l);
        case WM_MOUSEWHEEL:
            d->app->video_input(0,GET_WHEEL_DELTA_WPARAM(w)/float(WHEEL_DELTA)*.15f);return 0;
        case WM_KEYUP:
            d->consumeCharacter = false;
            return DefWindowProcW(window, message, w, l);
        case WM_PAINT: {
            PAINTSTRUCT p{};
            auto dc = BeginPaint(window, &p);
            d->draw();
            RECT r{};
            GetClientRect(window, &r);
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            float scale = std::max(.01f, std::min(r.right / 1440.f, r.bottom / float(height)));
            int width = int(1440 * scale), hh = int(height * scale);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = 1440;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, (r.right - width) / 2, (r.bottom - hh) / 2, width, hh, 0, 0, 1440, height,
                          d->pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(window, &p);
            return 0;
        }
        case WM_TIMER:
            d->tick();
            return 0;
        case WM_MOUSEMOVE:
            d->ui.pointer(0, d->point(l), d->pressed);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            d->pressed = true;
            SetCapture(window);
            d->ui.pointer(0, d->point(l), true);
            return 0;
        case WM_LBUTTONUP:
            d->pressed = false;
            ReleaseCapture();
            d->ui.pointer(0, d->point(l), false);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_CAPTURECHANGED:
            if (d->pressed) {
                d->pressed = false;
                d->ui.pointer(0, {-100, -100}, false);
            }
            return 0;
        case WM_CHAR:
            if (d->consumeCharacter) return 0;
            d->app->text(char(w));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SIZE:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, w, l);
        }
    } catch (const std::exception &e) {
        MessageBoxA(window, e.what(), "CyVR Player", MB_ICONERROR);
        PostQuitMessage(2);
        return 0;
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        SetProcessDPIAware();
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        auto directory = std::filesystem::path(path).parent_path();
        auto assets = directory / "assets";
        std::string root = std::filesystem::current_path().u8string(), snapshot, source, language="en";
        int page = 0, displayTab=0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--root" && i + 1 < argc)
                root = argv[++i];
            else if (arg == "--snapshot" && i + 1 < argc)
                snapshot = argv[++i];
            else if (arg == "--open" && i + 1 < argc)
                source = argv[++i];
            else if (arg == "--page" && i + 1 < argc)
                page = std::stoi(argv[++i]);
            else if (arg == "--language" && i + 1 < argc)
                language = argv[++i];
            else if(arg=="--display-tab"&&i+1<argc)displayTab=std::clamp(std::stoi(argv[++i]),0,2);
        }
        Desktop desktop;
        desktop.initialize(assets, root);
        desktop.ui.setLanguage(language);
        desktop.app->action(1000 + std::clamp(page, 0, 3));
        desktop.app->action(1300+displayTab);
        if (!source.empty())
            desktop.app->open(source);
        if (!snapshot.empty()) {
            for (int i = 0; i < 100; ++i) {
                desktop.app->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::vector<uint8_t> thumbnails;if(desktop.app->take_thumbnails(thumbnails))desktop.renderer.uploadImages(160,630,thumbnails);
            desktop.draw();
            std::ofstream out(snapshot, std::ios::binary);
            out << "P6\n1440 " << height << "\n255\n";
            for (size_t i = 0; i < desktop.pixels.size(); i += 4) {
                out.put(char(desktop.pixels[i + 2]));
                out.put(char(desktop.pixels[i + 1]));
                out.put(char(desktop.pixels[i]));
            }
            if (!out)
                throw std::runtime_error("Snapshot write failed");
            std::cout << "Vulkan snapshot: " << snapshot << '\n';
            return 0;
        }
        WNDCLASSW cls{};
        cls.lpfnWndProc = procedure;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"CyVRPlayer";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        auto window = CreateWindowExW(0, cls.lpszClassName, L"CyVR Player / CyVR_UI", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 1100, 900, nullptr, nullptr,
                                      cls.hInstance, &desktop);
        if (!window)
            throw std::runtime_error("Window creation failed");
        ShowWindow(window, SW_SHOW);
        SetTimer(window, 1, 16, nullptr);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return int(message.wParam);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
