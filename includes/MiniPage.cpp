// MiniPage.cpp - the small page shown in the upper-right corner while you walk with a narrated book
// ("read while walking", see AutoTurn.cpp). Drawn with Dear ImGui on top of the HUD.
//
// Hooks (same places powerofthree's Photo Mode uses):
//  - BSGraphics::InitD3D (SE 75595+0x9 / AE 77226+0x275): the call that creates the D3D11 device and swap chain;
//    ImGui is set up right after it.
//  - HUDMenu::PostDisplay (vtable index 6): the mini page is drawn after the HUD, so it sits under other menus and
//    disappears with the HUD.

// Only dxgi.h (for the swap-chain description): d3d11.h would clash with CommonLib's RE::ID3D11Buffer & co.
// under "using namespace RE", and Windows headers must come after CommonLib's own WinAPI declarations, so this file
// is included after the PCH. The D3D11 device/context are only passed on as pointers (declared by CommonLib/ImGui).
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <spdlog/spdlog.h>

namespace MiniPage {
    // ---- settings ([AutoTurn] in ReadItNow.ini) ----
    float fWidth = 0.25f;   // fraction of the screen width
    float fHeight = 0.5f;   // fraction of the screen height
    float fOpacity = 0.92f;

    // ---- what to show (written by AutoTurn on the main thread, read when drawing) ----
    struct View {
        bool show = false;
        std::string text;    // the page the narrator is on (UTF-8)
        std::string header;  // e.g. "Azura and the Box - page 3 of 6"
        bool paused = false;
    };
    std::mutex viewLock;
    View view;

    void Show(std::string a_header, std::string a_text, bool a_paused) {
        std::scoped_lock l(viewLock);
        view.show = true;
        view.header = std::move(a_header);
        view.text = std::move(a_text);
        view.paused = a_paused;
    }

    void SetPaused(bool a_paused) {
        std::scoped_lock l(viewLock);
        view.paused = a_paused;
    }

    void Hide() {
        std::scoped_lock l(viewLock);
        view.show = false;
    }

    std::atomic<bool> initialized{false};
    ImFont* bodyFont = nullptr;
    ImFont* headerFont = nullptr;

    void LoadFonts(float a_screenHeight) {
        auto& io = ImGui::GetIO();
        const float body = (std::max)(14.0f, a_screenHeight * 0.024f);
        const float head = (std::max)(12.0f, a_screenHeight * 0.017f);
        // Windows' Georgia: a book face that ships with every Windows; ImGui's own font if it isn't there
        static constexpr const char* fonts[] = {"C:\\Windows\\Fonts\\georgia.ttf", "C:\\Windows\\Fonts\\times.ttf"};
        for (auto f : fonts) {
            if (std::filesystem::exists(f)) {
                bodyFont = io.Fonts->AddFontFromFileTTF(f, body, nullptr, io.Fonts->GetGlyphRangesDefault());
                headerFont = io.Fonts->AddFontFromFileTTF(f, head, nullptr, io.Fonts->GetGlyphRangesDefault());
                if (bodyFont && headerFont) break;
            }
        }
        if (!bodyFont || !headerFont) {
            ImFontConfig cfg;
            cfg.SizePixels = body;
            bodyFont = io.Fonts->AddFontDefault(&cfg);
            cfg.SizePixels = head;
            headerFont = io.Fonts->AddFontDefault(&cfg);
        }
    }

    struct CreateD3DAndSwapChain {
        static void thunk() {
            func();
            try {
                auto renderer = RE::BSGraphics::Renderer::GetSingleton();
                if (!renderer) return;
                auto swapChain = reinterpret_cast<IDXGISwapChain*>(renderer->data.renderWindows[0].swapChain);
                if (!swapChain) {
                    spdlog::info("MiniPage: no swap chain, read-while-walking page disabled");
                    return;
                }
                DXGI_SWAP_CHAIN_DESC desc{};
                if (FAILED(swapChain->GetDesc(&desc))) {
                    spdlog::info("MiniPage: IDXGISwapChain::GetDesc failed, read-while-walking page disabled");
                    return;
                }
                auto device = reinterpret_cast<ID3D11Device*>(renderer->data.forwarder);
                auto context = reinterpret_cast<ID3D11DeviceContext*>(renderer->data.context);

                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                auto& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.LogFilename = nullptr;
                io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse;
                if (!ImGui_ImplWin32_Init(desc.OutputWindow) || !ImGui_ImplDX11_Init(device, context)) {
                    spdlog::info("MiniPage: ImGui set-up failed, read-while-walking page disabled");
                    return;
                }
                LoadFonts(static_cast<float>(desc.BufferDesc.Height ? desc.BufferDesc.Height : 1080));
                initialized = true;
                spdlog::info("MiniPage: ready ({}x{})", desc.BufferDesc.Width, desc.BufferDesc.Height);
            } catch (...) {
                spdlog::info("MiniPage: set-up error, read-while-walking page disabled");
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void Draw(const View& a_v) {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float w = screen.x * fWidth;
        const float h = screen.y * fHeight;
        const float margin = screen.y * 0.03f;
        ImGui::SetNextWindowPos(ImVec2(screen.x - w - margin, margin), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(fOpacity);

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.92f, 0.86f, 0.72f, 1.0f));  // parchment
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.42f, 0.30f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.12f, 0.06f, 1.0f));      // dark brown ink
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, screen.y * 0.006f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, (std::max)(1.0f, screen.y * 0.0015f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(w * 0.07f, h * 0.04f));

        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
        if (ImGui::Begin("##ReadItNowMiniPage", nullptr, flags)) {
            ImGui::PushFont(headerFont);
            ImGui::TextUnformatted(a_v.header.c_str());
            if (a_v.paused) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.08f, 0.05f, 1.0f), "  (paused)");
            }
            ImGui::PopFont();
            ImGui::Separator();

            // fit the page into the box: shrink the text if it's too long for it
            ImGui::PushFont(bodyFont);
            const float wrap = ImGui::GetContentRegionAvail().x;
            const float avail = ImGui::GetContentRegionAvail().y;
            float scale = 1.0f;
            for (int i = 0; i < 12; ++i) {
                ImGui::SetWindowFontScale(scale);
                const ImVec2 size = ImGui::CalcTextSize(a_v.text.c_str(), nullptr, false, wrap);
                if (size.y <= avail || scale <= 0.45f) break;
                scale *= 0.92f;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
            ImGui::TextUnformatted(a_v.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
    }

    struct PostDisplay {
        static void thunk(RE::IMenu* a_menu) {
            func(a_menu);
            if (!initialized) return;
            View v;
            {
                std::scoped_lock l(viewLock);
                if (!view.show) return;
                v = view;
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            Draw(v);
            ImGui::EndFrame();
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static inline constexpr std::size_t idx = 0x6;
    };

    // called from SKSEPluginLoad, before the game creates its D3D device
    void Install() {
        SKSE::AllocTrampoline(64);
        auto& trampoline = SKSE::GetTrampoline();
        REL::Relocation<std::uintptr_t> initD3D{RELOCATION_ID(75595, 77226), REL::VariantOffset(0x9, 0x275, 0x9)};
        CreateD3DAndSwapChain::func = trampoline.write_call<5>(initD3D.address(), CreateD3DAndSwapChain::thunk);

        REL::Relocation<std::uintptr_t> hudVtbl{RE::VTABLE_HUDMenu[0]};
        PostDisplay::func = hudVtbl.write_vfunc(PostDisplay::idx, PostDisplay::thunk);
    }
}
