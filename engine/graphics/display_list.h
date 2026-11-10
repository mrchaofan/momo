#pragma once

// DisplayList: 一段不可变、可重放、可跨线程共享的绘制指令(docs/05 §2)。
// 存储 = POD 指令流(DisplayList::Op, 参数为 float, 与 Skia 一致)+ 侧表(Paint、Path、Matrix、嵌套 DisplayList)。
// ui 线程由 DisplayListBuilder 记录(JS Canvas)→ Build() 冻结为 shared_ptr<const DisplayList>
// → raster 线程首次回放时转换为 Skia 对象并缓存(raster_cache), 之后每帧直接重放缓存。
//
// 线程约定: Builder 单线程使用;DisplayList 构建后不可变, 任意线程只读(缓存的惰性创建线程安全)。

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "base/geometry.h"
#include "base/macros.h"
#include "base/matrix.h"
#include "engine/graphics/image.h"
#include "engine/graphics/paint.h"
#include "engine/graphics/path.h"
#include "engine/graphics/texture.h"

namespace momo::engine::graphics {

// 由其他 Skia 模块(text: 段落)预先录制好的不可变绘制内容。定义在 graphics 内部头
// skia/paint_context.h(包装 SkPicture), 公共头只见前置声明。
struct ExternalPicture;

class DisplayList {
 public:
  enum class OpType : uint8_t {
    kSave,
    kRestore,
    kTranslate,        // a0=dx a1=dy
    kScale,            // a0=sx a1=sy
    kRotate,           // a0=radians
    kTransform,        // aux=matrix index
    kClipRect,         // a0..a3 = x y w h
    kDrawColor,        // color
    kDrawPaint,        // paint
    kDrawRect,         // paint, a0..a3
    kDrawRRect,        // paint, a0..a3, a4=radius
    kDrawOval,         // paint, a0..a3
    kDrawCircle,       // paint, a0=cx a1=cy a2=r
    kDrawLine,         // paint, a0=x0 a1=y0 a2=x1 a3=y1
    kDrawPath,         // paint, aux=path index
    kDrawDisplayList,  // aux=display list index
    kSaveLayer,        // paint(kNoPaint = 无), aux=1 有 bounds, a0..a3 = bounds
    kClipRRect,        // a0..a3 rect, a4 = radius
    kClipPath,         // aux=path index
    kDrawImage,        // paint(可 kNoPaint), aux=image index, a0=x a1=y
    kDrawImageRect,    // paint(可 kNoPaint), aux=image index, a0..a3 = dst, a4 = rects() 中 src 的索引
    kDrawExternal,     // aux=externals() index, a0=x a1=y
    kDrawTexture,      // aux=textures() index, a0..a3 = dst
  };

  static constexpr uint32_t kNoPaint = 0xFFFFFFFF;

  struct Op {
    OpType type;
    uint32_t paint = 0;  // paints() 索引(或 kDrawColor 的颜色)
    uint32_t aux = 0;    // paths()/matrices()/display_lists() 索引
    float a[5] = {0, 0, 0, 0, 0};
  };

  const std::vector<Op>& ops() const { return ops_; }
  const std::vector<Paint>& paints() const { return paints_; }
  const std::vector<std::shared_ptr<const Path>>& paths() const { return paths_; }
  const std::vector<Matrix>& matrices() const { return matrices_; }
  const std::vector<std::shared_ptr<const DisplayList>>& display_lists() const {
    return display_lists_;
  }
  const std::vector<std::shared_ptr<const Image>>& images() const { return images_; }
  const std::vector<Rect>& rects() const { return rects_; }
  const std::vector<std::shared_ptr<const ExternalPicture>>& externals() const {
    return externals_;
  }
  const std::vector<std::shared_ptr<Texture>>& textures() const { return textures_; }
  // 含外部纹理(自身或嵌套): 内容随纹理帧变化, 每次重放而不缓存为 SkPicture。
  bool is_dynamic() const { return dynamic_; }

  // 绘制内容在本地坐标系下的保守包围盒(已考虑记录期的变换与裁剪)。
  // 含无界绘制(drawColor/drawPaint)且无 cull rect 时为 nullopt。
  const std::optional<Rect>& bounds() const { return bounds_; }
  size_t op_count() const { return ops_.size(); }

  // 转换后的 Skia 对象(定义在 graphics 内部头 skia/paint_context.h, 外部不可用)。首次调用时创建。
  struct RasterCache;
  const RasterCache& raster_cache() const;

 private:
  friend class DisplayListBuilder;
  DisplayList() = default;

  std::vector<Op> ops_;
  std::vector<Paint> paints_;
  std::vector<std::shared_ptr<const Path>> paths_;
  std::vector<Matrix> matrices_;
  std::vector<std::shared_ptr<const DisplayList>> display_lists_;
  std::vector<std::shared_ptr<const Image>> images_;
  std::vector<Rect> rects_;
  std::vector<std::shared_ptr<const ExternalPicture>> externals_;
  std::vector<std::shared_ptr<Texture>> textures_;
  std::optional<Rect> bounds_;
  bool dynamic_ = false;

  mutable std::once_flag raster_cache_once_;
  mutable std::shared_ptr<const RasterCache> raster_cache_;
};

// 记录接口与 SkCanvas 对齐(子集)。坐标单位由调用方决定(框架层用物理像素)。
class DisplayListBuilder {
 public:
  // cull_rect: 可选的记录范围, 用于约束无界绘制的包围盒。
  explicit DisplayListBuilder(std::optional<Rect> cull_rect = std::nullopt);

  void Save();
  void Restore();  // 多余的 Restore 被忽略
  int save_count() const { return static_cast<int>(stack_.size()); }
  void Translate(double dx, double dy);
  void Scale(double sx, double sy);
  void Rotate(double radians);
  void Transform(const Matrix& matrix);
  void ClipRect(const Rect& rect);
  void ClipRRect(const Rect& rect, double radius);
  void ClipPath(std::shared_ptr<const Path> path);
  // 离屏图层;paint 为空表示普通图层(alpha 等取自 paint)。restore 时合成。
  void SaveLayer(const std::optional<Rect>& bounds, const Paint* paint);

  void DrawColor(uint32_t argb);
  void DrawPaint(const Paint& paint);
  void DrawRect(const Rect& rect, const Paint& paint);
  void DrawRRect(const Rect& rect, double radius, const Paint& paint);
  void DrawOval(const Rect& rect, const Paint& paint);
  void DrawCircle(Point center, double radius, const Paint& paint);
  void DrawLine(Point p0, Point p1, const Paint& paint);
  // 共享快照, 不拷贝路径数据(调用方之后不得修改 *path, 见 JsPath 的写时复制)。
  void DrawPath(std::shared_ptr<const Path> path, const Paint& paint);
  void DrawPath(const Path& path, const Paint& paint) {
    DrawPath(std::make_shared<const Path>(path), paint);
  }
  void DrawDisplayList(std::shared_ptr<const DisplayList> display_list);
  // 外环 outer 与内环 inner 两个圆角矩形之间的区域(以 even-odd 路径记录)。
  void DrawDRRect(const Rect& outer, double outer_radius, const Rect& inner, double inner_radius,
                  const Paint& paint);
  void DrawImage(std::shared_ptr<const Image> image, Point offset, const Paint* paint);
  void DrawImageRect(std::shared_ptr<const Image> image, const Rect& src, const Rect& dst,
                     const Paint* paint);
  // local_bounds: 内容在本地坐标(相对 offset)的包围盒。
  void DrawExternal(std::shared_ptr<const ExternalPicture> picture, Point offset,
                    const Rect& local_bounds);
  void DrawTexture(std::shared_ptr<Texture> texture, const Rect& dst);

  // 冻结并返回结果;未配平的 Save 自动 Restore。之后 builder 回到初始状态可复用。
  std::shared_ptr<const DisplayList> Build();

 private:
  struct State {
    Matrix matrix;
    std::optional<Rect> clip;  // 设备(记录根)坐标
  };

  uint32_t AddPaint(const Paint& paint);
  void ClipRectState(const Rect& local_rect);
  void Push(DisplayList::OpType type, uint32_t paint = 0, uint32_t aux = 0,
            std::initializer_list<double> args = {});
  // 把本地坐标的 rect(按 paint 描边外扩)累加进 bounds。
  void AccumulateLocal(const Rect& local, const Paint* paint);
  void AccumulateUnbounded();
  void AccumulateDevice(const Rect& device);

  std::optional<Rect> cull_rect_;
  std::unique_ptr<DisplayList> list_;
  State state_;
  std::vector<State> stack_;
  std::optional<Rect> bounds_;
  bool unbounded_ = false;

  MOMO_DISALLOW_COPY_AND_ASSIGN(DisplayListBuilder);
};

}  // namespace momo::engine::graphics
