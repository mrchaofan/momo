#pragma once

// graphics 内部头: Layer::Paint 的上下文与 Skia 转换。只能被 engine/graphics 的实现文件包含
// (Skia 类型不出 graphics, docs/10 §1)。

#include "engine/graphics/display_list.h"
#include "engine/graphics/image.h"
#include "engine/graphics/paint.h"
#include "engine/graphics/path.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPicture.h"
#include "include/core/SkSamplingOptions.h"

#include <optional>
#include <vector>

namespace momo::engine::graphics {

// 平台视图分片器(composition.cc 实现, docs/05 §9)。只在合成路径上存在。
class ViewSlicer {
 public:
  // 叶子即将在 leaf 上绘制 local_bounds(leaf 当前坐标系;nullopt = 无界), 用于计算各分片的内容范围。
  virtual void NoteLeafDraw(SkCanvas* leaf, const std::optional<Rect>& local_bounds) = 0;
  // 叶子的可命中区域(leaf 当前坐标系), 用于判断平台视图之上哪里的 momo 内容会接收指针事件。
  virtual void NoteHitRegions(SkCanvas* leaf, const std::vector<Rect>& local_regions) = 0;
  // 绘制遍历遇到一个平台视图: 返回之后的叶子应画入的分片画布。
  virtual SkCanvas* OnPlatformView() = 0;

 protected:
  ~ViewSlicer() = default;
};

struct PaintContext {
  // 容器层(变换/裁剪/saveLayer)操作的画布。合成路径上是一个 N 路画布, 把状态同步到所有分片。
  SkCanvas* canvas = nullptr;
  // 祖先 OpacityLayer 下推的透明度(0..255);只有 CanInheritOpacity() 的层会读取它。
  uint8_t inherited_alpha = 255;
  // 叶子层(Picture/Texture)绘制的画布: 合成路径上为当前分片, 否则为空(= canvas)。
  SkCanvas* leaf_canvas = nullptr;
  ViewSlicer* slicer = nullptr;

  SkCanvas* leaf() const { return leaf_canvas != nullptr ? leaf_canvas : canvas; }
};

SkPaint ToSkPaint(const Paint& paint);
SkPath ToSkPath(const Path& path);
SkRect ToSkRect(const Rect& rect);
SkM44 ToSkM44(const Matrix& matrix);

// DisplayList 一次性转换成的 SkPicture: SkPaint/SkShader/SkPath 只创建一次, 其 genID 跨帧稳定
// (GPU 后端的路径/细分缓存得以命中)。嵌套 DisplayList 引用各自缓存的 picture。
// 动态列表(含外部纹理, DisplayList::is_dynamic)不录制 picture, 每次按指令重放(paints 预先转换)。
struct DisplayList::RasterCache {
  sk_sp<SkPicture> picture;
  std::vector<SkPaint> paints;
};

struct Image::Impl {
  sk_sp<SkImage> image;  // raster(CPU)图像, 不可变
};

// text 等模块预先录制的不可变内容(见 display_list.h)。
struct ExternalPicture {
  sk_sp<SkPicture> picture;
};

SkSamplingOptions ToSkSampling(Paint::FilterQuality quality);
// 由 SkImage 构造 Image(assets 解码、纹理帧使用)。
std::shared_ptr<const Image> MakeImage(sk_sp<SkImage> image);

// 把 DisplayList 重放到 canvas(save/restore 配平, 不影响调用方状态)。
void RenderDisplayList(const DisplayList& list, SkCanvas* canvas);
// 同上, 但所有绘制的 alpha 乘以 alpha/255(仅用于不自重叠的列表, 见 PictureLayer::CanInheritOpacity)。
void RenderDisplayListWithAlpha(const DisplayList& list, SkCanvas* canvas, uint8_t alpha);

}  // namespace momo::engine::graphics
