#include "embedder/windows/win_caption_buttons.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>

#include "base/logging.h"

namespace momo::embedder::windows {

namespace {

constexpr wchar_t kClassName[] = L"MomoCaptionButtons";
// 字形(Segoe Fluent Icons / Segoe MDL2 Assets 共用码位)。
constexpr wchar_t kGlyphMinimize[] = L"\uE921";
constexpr wchar_t kGlyphMaximize[] = L"\uE922";
constexpr wchar_t kGlyphRestore[] = L"\uE923";
constexpr wchar_t kGlyphClose[] = L"\uE8BB";
constexpr double kGlyphSize = 10.0;  // 逻辑点

struct Rgba {
  double r = 0, g = 0, b = 0, a = 0;  // 非预乘, 0..1
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCHITTEST) {
    return HTTRANSPARENT;  // 鼠标交给父窗口的非客户区处理
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

RECT ToPhysical(const Rect& r, double dpr, double origin_x) {
  return {static_cast<LONG>(std::lround((r.x - origin_x) * dpr)), static_cast<LONG>(std::lround(r.y * dpr)),
          static_cast<LONG>(std::lround((r.right() - origin_x) * dpr)),
          static_cast<LONG>(std::lround(r.bottom() * dpr))};
}

// 预乘 BGRA 像素上的 source-over。
void Blend(uint32_t& dst, const Rgba& c, double coverage) {
  const double a = std::clamp(c.a * coverage, 0.0, 1.0);
  if (a <= 0) return;
  const double inv = 1.0 - a;
  const double db = (dst & 0xFF) / 255.0;
  const double dg = ((dst >> 8) & 0xFF) / 255.0;
  const double dr = ((dst >> 16) & 0xFF) / 255.0;
  const double da = ((dst >> 24) & 0xFF) / 255.0;
  auto to8 = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
  dst = to8(c.b * a + db * inv) | (to8(c.g * a + dg * inv) << 8) | (to8(c.r * a + dr * inv) << 16) |
        (to8(a + da * inv) << 24);
}

// 选一个装有标题栏字形的字体: Windows 11 的 Segoe Fluent Icons, 否则 Windows 10 的 Segoe MDL2 Assets。
HFONT CreateGlyphFont(int pixel_height, HDC dc) {
  for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
    HFONT font = ::CreateFontW(-pixel_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, face);
    if (font == nullptr) continue;
    HGDIOBJ old = ::SelectObject(dc, font);
    wchar_t actual[LF_FACESIZE] = {};
    ::GetTextFaceW(dc, LF_FACESIZE, actual);
    ::SelectObject(dc, old);
    if (std::wcscmp(actual, face) == 0) {
      return font;  // 字体映射没有替换成别的字体
    }
    ::DeleteObject(font);
  }
  return nullptr;
}

}  // namespace

WinCaptionButtons::WinCaptionButtons(HWND parent) : parent_(parent) {
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &WndProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClassName;
  if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    MOMO_LOG(ERROR) << "RegisterClassExW(caption buttons) failed: " << ::GetLastError();
    return;
  }
  hwnd_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kClassName, L"",
                            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent_, nullptr, instance, nullptr);
  if (hwnd_ == nullptr) {
    MOMO_LOG(ERROR) << "CreateWindowExW(caption buttons) failed: " << ::GetLastError();
  }
}

WinCaptionButtons::~WinCaptionButtons() {
  if (hwnd_ != nullptr) {
    ::DestroyWindow(hwnd_);
  }
}

void WinCaptionButtons::Update(const WinCaptionButtonsAppearance& appearance) {
  if (hwnd_ == nullptr || (painted_ && appearance == appearance_)) {
    return;
  }
  appearance_ = appearance;
  painted_ = true;
  if (!appearance_.visible || appearance_.layout.empty()) {
    ::ShowWindow(hwnd_, SW_HIDE);
    return;
  }
  Paint();
}

void WinCaptionButtons::Paint() {
  const WinCaptionButtonsAppearance& a = appearance_;
  const double dpr = a.dpr > 0 ? a.dpr : 1.0;
  const Rect& bounds = a.layout.bounds;
  const RECT whole = ToPhysical(bounds, dpr, 0);
  const int width = whole.right - whole.left;
  const int height = whole.bottom - whole.top;
  if (width <= 0 || height <= 0) {
    ::ShowWindow(hwnd_, SW_HIDE);
    return;
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // 自上而下
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  HDC screen = ::GetDC(nullptr);
  HDC dc = ::CreateCompatibleDC(screen);
  HDC mask_dc = ::CreateCompatibleDC(screen);
  void* bits = nullptr;
  void* mask_bits = nullptr;
  HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HBITMAP mask = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &mask_bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  if (dc == nullptr || mask_dc == nullptr || bitmap == nullptr || mask == nullptr) {
    if (bitmap) ::DeleteObject(bitmap);
    if (mask) ::DeleteObject(mask);
    if (dc) ::DeleteDC(dc);
    if (mask_dc) ::DeleteDC(mask_dc);
    return;
  }
  HGDIOBJ old_bitmap = ::SelectObject(dc, bitmap);
  HGDIOBJ old_mask = ::SelectObject(mask_dc, mask);
  auto* px = static_cast<uint32_t*>(bits);
  auto* mask_px = static_cast<uint32_t*>(mask_bits);
  std::fill(px, px + static_cast<size_t>(width) * height, 0u);  // 全透明
  std::fill(mask_px, mask_px + static_cast<size_t>(width) * height, 0u);

  // 字形: 白字黑底画进遮罩, 覆盖率取自遮罩的灰度(灰度抗锯齿, 不用 ClearType)。
  HFONT font = CreateGlyphFont(static_cast<int>(std::lround(kGlyphSize * dpr)), mask_dc);
  HGDIOBJ old_font = font != nullptr ? ::SelectObject(mask_dc, font) : nullptr;
  ::SetBkMode(mask_dc, TRANSPARENT);
  ::SetTextColor(mask_dc, RGB(255, 255, 255));

  const Rgba glyph_color = a.dark ? Rgba{1, 1, 1, a.active ? 1.0 : 0.45} : Rgba{0, 0, 0, a.active ? 0.9 : 0.4};
  const Rgba hover_bg = a.dark ? Rgba{1, 1, 1, 0.08} : Rgba{0, 0, 0, 0.06};
  const Rgba pressed_bg = a.dark ? Rgba{1, 1, 1, 0.14} : Rgba{0, 0, 0, 0.12};
  const Rgba close_hover_bg{0xC4 / 255.0, 0x2B / 255.0, 0x1C / 255.0, 1.0};
  const Rgba close_pressed_bg{0xC4 / 255.0, 0x2B / 255.0, 0x1C / 255.0, 0.85};

  struct Button {
    WinFrameHit hit;
    Rect rect;
    const wchar_t* glyph;
    bool enabled;
  };
  const Button buttons[] = {
      {WinFrameHit::kMinimizeButton, a.layout.minimize, kGlyphMinimize, true},
      {WinFrameHit::kMaximizeButton, a.layout.maximize, a.maximized ? kGlyphRestore : kGlyphMaximize,
       a.layout.maximize_enabled},
      {WinFrameHit::kCloseButton, a.layout.close, kGlyphClose, true},
  };
  for (const Button& b : buttons) {
    RECT r = ToPhysical(b.rect, dpr, bounds.x);
    r.left = std::clamp<LONG>(r.left, 0, width);
    r.right = std::clamp<LONG>(r.right, 0, width);
    r.top = std::clamp<LONG>(r.top, 0, height);
    r.bottom = std::clamp<LONG>(r.bottom, 0, height);
    const bool hovered = b.enabled && a.hover == b.hit;
    const bool pressed = b.enabled && a.pressed == b.hit && hovered;
    const bool close = b.hit == WinFrameHit::kCloseButton;
    if (hovered || pressed) {
      const Rgba bg = close ? (pressed ? close_pressed_bg : close_hover_bg) : (pressed ? pressed_bg : hover_bg);
      for (LONG y = r.top; y < r.bottom; ++y) {
        for (LONG x = r.left; x < r.right; ++x) Blend(px[static_cast<size_t>(y) * width + x], bg, 1.0);
      }
    }
    if (font == nullptr) continue;
    ::DrawTextW(mask_dc, b.glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::GdiFlush();
    Rgba color = (close && hovered) ? Rgba{1, 1, 1, 1} : glyph_color;
    if (!b.enabled) color.a *= 0.35;
    for (LONG y = r.top; y < r.bottom; ++y) {
      for (LONG x = r.left; x < r.right; ++x) {
        const size_t i = static_cast<size_t>(y) * width + x;
        const uint32_t m = mask_px[i];
        const double coverage = std::max({m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF}) / 255.0;
        Blend(px[i], color, coverage);
      }
    }
  }
  if (old_font != nullptr) ::SelectObject(mask_dc, old_font);
  if (font != nullptr) ::DeleteObject(font);

  // 先定位(子窗口坐标相对父窗口内容区), 再更新分层内容。
  ::SetWindowPos(hwnd_, HWND_TOP, whole.left, whole.top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW);
  SIZE size{width, height};
  POINT source{0, 0};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  if (!::UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, dc, &source, 0, &blend, ULW_ALPHA)) {
    MOMO_LOG(WARNING) << "UpdateLayeredWindow(caption buttons) failed: " << ::GetLastError();
  }

  ::SelectObject(dc, old_bitmap);
  ::SelectObject(mask_dc, old_mask);
  ::DeleteObject(bitmap);
  ::DeleteObject(mask);
  ::DeleteDC(dc);
  ::DeleteDC(mask_dc);
}

}  // namespace momo::embedder::windows
