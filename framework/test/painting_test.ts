import {
  Align, Alignment, AssetImage, BorderRadius, BoxDecoration, BoxShadow, Center, ClipRRect, Color, Colors, Container,
  DecoratedBox, Image, ImageCache, ImageStreamCompleter, MemoryImage, Offset, Opacity, RepaintBoundary,
  SizedBox, Stack, TextPainter, TextSpan, TextStyle, Transform, Matrix4, paragraphCache, imageCache, OffsetLayer, OpacityLayer,
  PictureLayer, TransformLayer, ClipRRectLayer, ContainerLayer, Texture, TextureLayer, CustomPaint, CustomPainter, Size,
  State, StatefulWidget, GlobalKey, type Widget, type Layer, RawImage, RenderImage, Positioned,
} from "../packages/momo/src/index";
import { Paint, type Canvas } from "momo:ui";
import { expect, group, test } from "./harness";
import type { RecordedOp } from "./host";
import { testWidgets } from "./util";


function layerKinds(layer: Layer, depth = 0, out: string[] = []): string[] {
  out.push(`${"  ".repeat(depth)}${layer.constructor.name}`);
  if (layer instanceof ContainerLayer) for (const c of layer.children) layerKinds(c, depth + 1, out);
  return out;
}

group("painting/TextPainter", () => {
  test("layout cache keyed by (text, style, width)", (ctx) => {
    paragraphCache.clear();
    const built0 = ctx.host.mock!.paragraphsBuilt;
    const span = (s: string): TextSpan => new TextSpan({ text: s, style: new TextStyle({ fontSize: 10 }) });
    const a = new TextPainter({ text: span("hello") });
    a.layout(0, 200);
    const b = new TextPainter({ text: span("hello") });
    b.layout(0, 200);
    expect.equal(ctx.host.mock!.paragraphsBuilt - built0, 1, "same text/style/width reuses the paragraph");
    expect.equal(a.paragraph, b.paragraph);
    b.layout(0, 30);
    expect.equal(ctx.host.mock!.paragraphsBuilt - built0, 2, "different width → new layout");
    const c = new TextPainter({ text: new TextSpan({ text: "hello", style: new TextStyle({ fontSize: 12 }) }) });
    c.layout(0, 200);
    expect.equal(ctx.host.mock!.paragraphsBuilt - built0, 3, "different style → new layout");
    a.layout(0, 200);
    expect.equal(ctx.host.mock!.paragraphsBuilt - built0, 3, "re-layout with same constraints is a no-op");
    expect.isTrue(paragraphCache.hits >= 1);
  }, { mockOnly: true });

  test("metrics, caret and selection boxes (Ahem)", () => {
    const p = new TextPainter({ text: new TextSpan({ text: "ab\ncd", style: new TextStyle({ fontSize: 10 }) }) });
    p.layout();
    expect.deepEqual([p.width, p.height], [20, 20]);
    expect.equal(p.preferredLineHeight, 10);
    expect.deepEqual(p.getOffsetForCaret(1).toString(), new Offset(10, 0).toString());
    expect.deepEqual(p.getOffsetForCaret(3).toString(), new Offset(0, 10).toString());
    expect.deepEqual(p.getOffsetForCaret(5).toString(), new Offset(20, 10).toString());
    const boxes = p.getBoxesForSelection(1, 4);
    expect.deepEqual(boxes.map((r) => [r.left, r.top, r.right, r.bottom]), [[10, 0, 20, 10], [0, 10, 10, 20]]);
    expect.equal(p.getPositionForOffset(new Offset(14, 15)).offset, 4);
    const centered = new TextPainter({ text: new TextSpan({ text: "ab", style: new TextStyle({ fontSize: 10 }) }), textAlign: "center" });
    centered.layout(100, 100);
    expect.equal(centered.width, 100);
    expect.equal(centered.getBoxesForSelection(0, 1)[0]!.left, 40);
  }, { ahemText: true });

  test("TextStyle merge / inherit", () => {
    const base = new TextStyle({ fontSize: 14, color: Colors.black });
    const m = base.merge(new TextStyle({ color: Colors.red }));
    expect.equal(m.fontSize, 14);
    expect.equal(m.color, Colors.red);
    const ni = base.merge(new TextStyle({ inherit: false, fontSize: 9 }));
    expect.equal(ni.color, undefined);
    expect.deepEqual(m.toUi(), { color: Colors.red.value, fontSize: 14 });
  });
});

group("painting/images", () => {
  test("ImageCache is an LRU bounded by count", () => {
    const cache = new ImageCache();
    cache.maximumSize = 2;
    const mk = (): ImageStreamCompleter => new ImageStreamCompleter();
    cache.putIfAbsent("a", mk);
    cache.putIfAbsent("b", mk);
    cache.putIfAbsent("a", mk); // 访问 a → b 最旧
    cache.putIfAbsent("c", mk);
    expect.isTrue(cache.containsKey("a"));
    expect.isFalse(cache.containsKey("b"));
    expect.isTrue(cache.containsKey("c"));
  });

  testWidgets("Image.asset loads via loadAsset + decodeImage and is cached", async (t, ctx) => {
    const m = ctx.host.mock!;
    const bytes = new Uint8Array(8);
    new DataView(bytes.buffer).setUint32(0, 40, true);
    new DataView(bytes.buffer).setUint32(4, 20, true);
    m.assets.set("img/logo.png", bytes);
    const decoded0 = m.decodedImages;
    await t.pumpWidget(new Align({ alignment: Alignment.topLeft, child: Image.asset("img/logo.png") }));
    await t.flushMicrotasks();
    await t.pump();
    const ro = t.one(t.byType(RawImage)).findRenderObject() as RenderImage;
    expect.deepEqual([ro.size.width, ro.size.height], [40, 20]);
    expect.notNull(ro.image);
    await t.pumpWidget(new Align({ alignment: Alignment.topLeft, child: Image.asset("img/logo.png", { width: 20 }) }));
    await t.flushMicrotasks();
    expect.equal(m.decodedImages - decoded0, 1, "second use hits the ImageCache");
    expect.isTrue(imageCache.containsKey(new AssetImage("img/logo.png").cacheKey));
    const r2 = t.one(t.byType(RawImage)).findRenderObject() as RenderImage;
    expect.deepEqual([r2.size.width, r2.size.height], [20, 10], "keeps aspect ratio");
    const mem = new MemoryImage(bytes);
    expect.isTrue(mem.equals(new MemoryImage(bytes)));
    expect.isFalse(mem.equals(new MemoryImage(new Uint8Array(8))));
  }, { mockOnly: true });
});

class Painter extends CustomPainter {
  paints = 0;
  paint(canvas: Canvas, size: Size): void {
    this.paints++;
    canvas.drawLine(0, 0, size.width, size.height, new Paint());
  }
  override shouldRepaint(): boolean {
    return false;
  }
}

class Toggle extends StatefulWidget {
  constructor(readonly builder: (v: number) => Widget, key: GlobalKey) {
    super(key);
  }
  createState(): State {
    return new ToggleState();
  }
}
class ToggleState extends State<Toggle> {
  v = 0;
  build(): Widget {
    return this.widget.builder(this.v);
  }
}

group("rendering/layers → SceneBuilder", () => {
  testWidgets("root TransformLayer scales by devicePixelRatio; scene ops map 1:1", async (t, ctx) => {
    await t.pumpWidget(new Stack({
      children: [
        new Positioned({ left: 10, top: 10, child: new Opacity({ opacity: 0.5, child: new Container({ width: 20, height: 20, color: Colors.red }) }) }),
        new Positioned({ left: 50, top: 0, child: new RepaintBoundary({ child: new Container({ width: 20, height: 20, color: Colors.blue }) }) }),
        new Positioned({ left: 100, top: 0, child: new ClipRRect({ borderRadius: BorderRadius.circular(4), child: new RepaintBoundary({ child: new SizedBox({ width: 30, height: 30 }) }) }) }),
        new Positioned({ left: 200, top: 0, width: 64, height: 48, child: new Texture({ textureId: 7 }) }),
      ],
    }));
    const root = t.binding.renderView.rootLayer!;
    expect.isTrue(root instanceof TransformLayer);
    expect.deepEqual(Array.from(root.transform.storage), [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    const kinds = layerKinds(root);
    expect.isTrue(kinds.some((k) => k.includes("OpacityLayer")));
    expect.isTrue(kinds.some((k) => k.includes("ClipRRectLayer")), kinds.join("\n"));
    expect.isTrue(kinds.some((k) => k.includes("TextureLayer")));
    const ops = ctx.host.mock!.lastSceneOps.map((o: RecordedOp) => o.op);
    expect.equal(ops[0], "pushTransform");
    expect.deepEqual((ctx.host.mock!.lastSceneOps[0]!.args[0] as number[]).slice(0, 6), [2, 0, 0, 0, 0, 2]);
    expect.isTrue(ops.includes("pushOpacity"));
    const op = ctx.host.mock!.lastSceneOps.find((o) => o.op === "pushOpacity")!;
    expect.equal(op.args[0], 128);
    const clip = ctx.host.mock!.lastSceneOps.find((o) => o.op === "pushClipRRect")!;
    expect.deepEqual(clip.args, [100, 0, 30, 30, 4]);
    const tex = ctx.host.mock!.lastSceneOps.find((o) => o.op === "addTexture")!;
    expect.deepEqual(tex.args, [7, 200, 0, 64, 48]);
    const pushes = ops.filter((o) => o.startsWith("push")).length;
    expect.equal(pushes, ops.filter((o) => o === "pop").length, "balanced push/pop");
    void OffsetLayer;
    void OpacityLayer;
    void ClipRRectLayer;
    void TextureLayer;
    void PictureLayer;
  }, { mockOnly: true, dpr: 2 });

  testWidgets("RepaintBoundary isolates repaints (picture reused)", async (t) => {
    const painter = new Painter();
    const key = new GlobalKey<ToggleState>();
    await t.pumpWidget(new Stack({
      children: [
        new RepaintBoundary({ child: new CustomPaint({ painter, size: new Size(10, 10) }) }),
        new Positioned({ left: 20, top: 0, child: new Toggle((v) => new Container({ width: 10, height: 10, color: v === 0 ? Colors.red : Colors.green }), key) }),
      ],
    }));
    expect.equal(painter.paints, 1);
    key.currentState!.setState(() => (key.currentState!.v = 1));
    await t.pump();
    expect.equal(painter.paints, 1, "sibling repaint does not repaint the boundary");
  });

  testWidgets("BoxDecoration: shadow uses Paint.maskBlurSigma; rounded rect", async (t, ctx) => {
    await t.pumpWidget(new Center({
      child: new DecoratedBox({
        decoration: new BoxDecoration({
          color: new Color(0xff112233),
          borderRadius: BorderRadius.circular(8),
          boxShadow: [new BoxShadow({ color: new Color(0x40000000), blurRadius: 10, offset: new Offset(0, 2) })],
        }),
        child: new SizedBox({ width: 100, height: 50 }),
      }),
    }));
    const pictures = ctx.host.mock!.lastSceneOps.filter((o) => o.op === "addPicture").map((o) => ctx.host.mock!.pictureOps(o.args[2]));
    const all = pictures.flat();
    const rrects = all.filter((o) => o.op === "drawRRect");
    expect.equal(rrects.length, 2);
    const shadowPaint = rrects[0]!.args[5] as { maskBlurSigma: number; color: number };
    expect.close(shadowPaint.maskBlurSigma, 10 * 0.57735 + 0.5, 1e-9);
    expect.deepEqual(rrects[0]!.args.slice(0, 5), [350, 277, 100, 50, 8]);
    expect.equal((rrects[1]!.args[5] as { color: number }).color, 0xff112233);
    void Transform;
    void Matrix4;
  }, { mockOnly: true });
});
