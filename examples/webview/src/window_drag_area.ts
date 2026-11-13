// 可拖动窗口的区域(业务层实现: 框架只提供 Widget.startDrag(sequenceId), 哪里可拖由应用决定)。
// 和普通手势一样经命中测试与手势竞技场: 主键按下命中这里时以一名成员加入竞技场, 赢了就以这次按下的序列
// 让宿主开始系统的窗口拖动(双击按系统设置缩放/最小化)。
//   - 画在它之上的内容(菜单、对话框)先命中, 按下到不了这里;
//   - 子节点里的手势(按钮等, 命中路径上更深、先加入竞技场)与它竞争: 点击归按钮, 拖出点击容差后按钮放弃,
//     拖动区域获胜;纯展示的子节点(标题文字)不参与竞争, 照样可以拖动窗口;
//   - 没有子节点竞争时立即认领这次按下: 祖先上的手势(整条标签栏的拖动识别器)不会把窗口拖动抢走——
//     与"拖动区域的像素归窗口"的系统惯例一致。
import {
  GestureBinding, PointerDownEvent, RenderProxyBox, SingleChildRenderObjectView, Size, Widget, kPrimaryButton,
  type BoxConstraints, type GestureArenaMember, type HitTestEntry, type Key, type Offset, type PointerEvent, type View,
} from "@momo/framework";

/** 一次按下在竞技场里的拖动请求: 获胜时让宿主以这次按下的序列开始窗口拖动。 */
class WindowDragRequest implements GestureArenaMember {
  constructor(readonly viewId: number, readonly sequenceId: number) {}
  acceptGesture(_pointer: number): void {
    Widget.fromId(this.viewId)?.startDrag(this.sequenceId);
  }
  rejectGesture(_pointer: number): void {}
}

class RenderWindowDragArea extends RenderProxyBox {
  protected override computeSizeForNoChild(constraints: BoxConstraints): Size {
    return constraints.biggest;
  }
  override hitTestSelf(_position: Offset): boolean {
    return true;
  }
  override handleEvent(event: PointerEvent, _entry: HitTestEntry): void {
    if (event instanceof PointerDownEvent && event.buttons === kPrimaryButton && event.sequenceId !== 0) {
      const arena = GestureBinding.instance.gestureArena;
      // 先加入的成员都是子节点的手势: 有则与它们公平竞争;没有则立即认领(祖先的手势之后加入, 一律落败)。
      const contested = arena.memberCount(event.pointer) > 0;
      const entry = arena.add(event.pointer, new WindowDragRequest(event.viewId, event.sequenceId));
      if (!contested) entry.resolve("accepted");
    }
  }
}

/** 可拖动窗口的区域。无子节点时填满约束(常用于顶栏的空白处: Expanded(WindowDragArea()))。 */
export class WindowDragArea extends SingleChildRenderObjectView {
  constructor(o: { key?: Key | null; child?: View | null } = {}) {
    super(o);
  }
  createRenderObject(): RenderWindowDragArea {
    return new RenderWindowDragArea();
  }
}
