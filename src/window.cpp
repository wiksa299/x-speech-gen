// Panel Graphics wrapper adapted from X-Speech Rekon (MIT, copyright 2026 wiksa).
#include "window.hpp"

#include <XPLMPanelGraphics.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <utility>
#include <vector>

namespace xsg {

namespace {
class Context {
public:
    explicit Context(ImGuiContext* context) : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(context); }
    ~Context() { ImGui::SetCurrentContext(previous_); }
private:
    ImGuiContext* previous_;
};
}

Window::Window(std::function<void()> draw, int width, int height, const std::filesystem::path& fallback_font,
               bool section_headings)
    : draw_content_(std::move(draw)) {
    const auto previous = ImGui::GetCurrentContext();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(previous);
    {
        Context context(imgui_);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        ImFontConfig font;
        font.SizePixels = 16;
        static constexpr ImWchar ranges[] = {0x0020, 0x052f, 0x2000, 0x206f, 0};
        std::filesystem::path font_path;
        if (!fallback_font.empty()) {
            char simulator_path[1024]{};
            XPLMGetSystemPath(simulator_path);
            const auto roboto = std::filesystem::u8path(simulator_path) / "Resources/fonts/Roboto-Regular.ttf";
            for (const auto& path : {roboto, fallback_font}) {
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error) &&
                    io.Fonts->AddFontFromFileTTF(path.u8string().c_str(), font.SizePixels, &font, ranges)) {
                    font_path = path;
                    break;
                }
            }
        }
        if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault(&font);
        if (section_headings) {
            font.SizePixels = 18;
            heading_font_ = font_path.empty() ? io.Fonts->AddFontDefault(&font) :
                io.Fonts->AddFontFromFileTTF(font_path.u8string().c_str(), font.SizePixels, &font, ranges);
        }
        auto& style = ImGui::GetStyle();
        style.WindowPadding = {16, 14};
        style.FramePadding = {9, 5};
        style.ItemSpacing = {9, 8};
        style.CellPadding = {10, 6};
        style.FrameRounding = 3;
        style.WindowRounding = 0;
        style.Colors[ImGuiCol_WindowBg] = {0.14f, 0.14f, 0.15f, 1};
        style.Colors[ImGuiCol_ChildBg] = {0.10f, 0.10f, 0.11f, 1};
        style.Colors[ImGuiCol_Text] = {0.95f, 0.96f, 0.98f, 1};
        style.Colors[ImGuiCol_TextDisabled] = {0.64f, 0.67f, 0.72f, 1};
        style.Colors[ImGuiCol_Header] = {0.18f, 0.27f, 0.40f, 1};
        style.Colors[ImGuiCol_Button] = {0.19f, 0.29f, 0.44f, 1};
    }

    int left, top, right, bottom;
    XPLMGetScreenBoundsGlobal(&left, &top, &right, &bottom);
    XPLMCreateWindow_t params{};
    params.structSize = sizeof(params);
    params.left = left + 55;
    params.top = top - 75;
    params.right = std::min(right - 12, params.left + width);
    params.bottom = std::max(bottom + 12, params.top - height);
    params.visible = 0;
    params.drawWindowFunc = draw_cb;
    params.handleMouseClickFunc = click_cb;
    params.handleKeyFunc = key_cb;
    params.handleCursorFunc = cursor_cb;
    params.handleMouseWheelFunc = wheel_cb;
    params.refcon = this;
    params.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    params.layer = xplm_WindowLayerFloatingWindows;
    params.contentType = xplm_WindowContentTypePanelGraphics;
    window_ = XPLMCreateWindowEx(&params);
}

Window::~Window() {
    if (window_) { hide(); XPLMDestroyWindow(window_); }
    if (texture_) XPLMDestroyTexture(texture_);
    if (imgui_) ImGui::DestroyContext(imgui_);
}

bool Window::visible() const { return window_ && XPLMGetWindowIsVisible(window_); }

void Window::section_heading(const char* text) const {
    ImGui::PushFont(heading_font_);
    ImGui::PushStyleColor(ImGuiCol_Text, {0.50f, 0.74f, 1.0f, 1.0f});
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Window::show(const char* title) {
    if (!window_) return;
    set_title(title);
    XPLMSetWindowIsVisible(window_, 1);
    XPLMBringWindowToFront(window_);
}

void Window::set_title(const char* title) {
    if (window_) XPLMSetWindowTitle(window_, title);
}

void Window::hide() {
    if (!window_) return;
    Context context(imgui_);
    mouse_down_ = false;
    if (keyboard_) XPLMTakeKeyboardFocus(nullptr);
    keyboard_ = false;
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    XPLMSetWindowIsVisible(window_, 0);
}

void Window::position(int x, int y) {
    int left, top, right, bottom;
    XPLMGetWindowGeometry(window_, &left, &top, &right, &bottom);
    ImGui::GetIO().AddMousePosEvent(float(x - left), float(top - y));
}

void Window::draw_cb(XPLMWindowID, void* data) {
    try { static_cast<Window*>(data)->draw(); }
    catch (const std::exception&) { XPLMDebugString("X-Speech Gen: window rendering failed\n"); }
}

int Window::click_cb(XPLMWindowID, int x, int y, XPLMMouseStatus status, void* data) {
    auto& self = *static_cast<Window*>(data);
    Context context(self.imgui_);
    self.position(x, y);
    if (status != xplm_MouseDrag) {
        self.mouse_down_ = status == xplm_MouseDown;
        ImGui::GetIO().AddMouseButtonEvent(0, self.mouse_down_);
    }
    return 1;
}

int Window::wheel_cb(XPLMWindowID, int x, int y, int wheel, int clicks, void* data) {
    auto& self = *static_cast<Window*>(data);
    Context context(self.imgui_);
    self.position(x, y);
    ImGui::GetIO().AddMouseWheelEvent(wheel == 1 ? float(clicks) : 0, wheel == 0 ? float(clicks) : 0);
    return 1;
}

void Window::key_cb(XPLMWindowID, char character, XPLMKeyFlags flags, char key, void* data, int losing) {
    auto& self = *static_cast<Window*>(data);
    Context context(self.imgui_);
    auto& io = ImGui::GetIO();
    if (losing) {
        self.keyboard_ = false;
        io.AddFocusEvent(false);
        return;
    }
    const bool down = (flags & xplm_UpFlag) == 0;
    io.AddKeyEvent(ImGuiMod_Ctrl, (flags & xplm_ControlFlag) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (flags & xplm_ShiftFlag) != 0);
    constexpr std::pair<int, ImGuiKey> keys[] = {
        {XPLM_VK_BACK, ImGuiKey_Backspace}, {XPLM_VK_DELETE, ImGuiKey_Delete},
        {XPLM_VK_LEFT, ImGuiKey_LeftArrow}, {XPLM_VK_RIGHT, ImGuiKey_RightArrow},
        {XPLM_VK_UP, ImGuiKey_UpArrow}, {XPLM_VK_DOWN, ImGuiKey_DownArrow},
        {XPLM_VK_HOME, ImGuiKey_Home}, {XPLM_VK_END, ImGuiKey_End},
        {XPLM_VK_RETURN, ImGuiKey_Enter}, {XPLM_VK_ESCAPE, ImGuiKey_Escape},
        {'A', ImGuiKey_A}, {'C', ImGuiKey_C}, {'V', ImGuiKey_V}, {'X', ImGuiKey_X},
        {'Y', ImGuiKey_Y}, {'Z', ImGuiKey_Z}
    };
    for (const auto& entry : keys) {
        if (entry.first == static_cast<unsigned char>(key)) io.AddKeyEvent(entry.second, down);
    }
    if (down && !(flags & xplm_ControlFlag) && static_cast<unsigned char>(character) >= 32 && character != 127) {
        io.AddInputCharacter(static_cast<unsigned char>(character));
    }
}

void Window::draw() {
    Context context(imgui_);
    auto& io = ImGui::GetIO();
    if (!texture_) {
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        texture_ = XPLMCreateTexture(pixels, width, height);
        if (!texture_) return;
        io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(texture_));
    }
    int left, top, right, bottom, x, y;
    XPLMGetWindowGeometry(window_, &left, &top, &right, &bottom);
    XPLMGetMouseLocationGlobal(&x, &y);
    position(x, y);
    io.DisplaySize = {float(right - left), float(top - bottom)};
    io.DeltaTime = std::max(0.001f, XPLMGetElapsedTime() - frame_time_);
    frame_time_ = XPLMGetElapsedTime();
    io.DeltaTime = std::min(io.DeltaTime, 0.1f);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr auto flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##xsg", nullptr, flags);
    try { draw_content_(); }
    catch (const std::exception&) {
        const bool recovery_asserts = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::EndFrame();
        io.ConfigErrorRecoveryEnableAssert = recovery_asserts;
        throw;
    }
    ImGui::End();
    ImGui::Render();
    if (io.WantTextInput != keyboard_) {
        keyboard_ = io.WantTextInput;
        XPLMTakeKeyboardFocus(keyboard_ ? window_ : nullptr);
        if (keyboard_) io.AddFocusEvent(true);
    }

    const auto* data = ImGui::GetDrawData();
    static_assert(sizeof(ImDrawVert) == 20 && offsetof(ImDrawVert, col) == 16);
    static_assert(sizeof(ImDrawIdx) == sizeof(uint16_t));
    XPLMTransformPush();
    struct RestoreTransform { ~RestoreTransform() { XPLMTransformPop(); } } restore_transform;
    XPLMTransformTranslate(float(left), float(top));
    XPLMTransformScale(1.0f, -1.0f);
    std::vector<XPLMDrawCall_t> calls;
    for (int i = 0; i < data->CmdListsCount; ++i) {
        const auto& list = *data->CmdLists[i];
        calls.clear();
        for (const auto& command : list.CmdBuffer) {
            if (command.UserCallback) continue;
            calls.push_back({reinterpret_cast<void*>(command.GetTexID()),
                {command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w},
                int(command.IdxOffset), int(command.ElemCount), int(command.VtxOffset)});
        }
        XPLMMesh_t mesh{list.VtxBuffer.Size, reinterpret_cast<const float*>(list.VtxBuffer.Data),
                        list.IdxBuffer.Size, list.IdxBuffer.Data};
        XPLMDrawCalls(&mesh, int(calls.size()), calls.data());
    }
}

} // namespace xsg
