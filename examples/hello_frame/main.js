// examples/hello_frame — M1 验收示例: 直接用 momo:ui 底层 API 画帧(docs/11 M1)。
// 渐变圆角卡片 + 旋转的路径星形 + 描边图形;每帧重新请求下一帧, 驱动整条帧管线:
//   onBeginFrame → onDrawFrame → view.render → Animator/Pipeline → raster 软件光栅 → present。
// 坐标以逻辑像素书写, 场景根部按 devicePixelRatio 缩放到物理像素(与 Flutter 相同的分工)。
// 分工: momo:ui 只管画面与输入(platformDispatcher = 全局帧回调, viewports = 视图);窗口标题等窗口属性属于宿主,
// 经宿主注册的 momo/window 通道设置(StandardMethodCodec, 这里手写最小编码)。
import { platformDispatcher, viewports, PictureRecorder, Paint, Path, Gradient, SceneBuilder } from 'momo:ui';
import { send, setHandler } from 'momo:services';

// 隐式视图: 宿主启动时创建的窗口(bundle 执行前即存在)。
const view = viewports.get(0);

// StandardMethodCodec 最小编码: string 与 {string: string | int32} 的 map。
function utf8(s) { return [...new TextEncoder().encode(s)]; }
function str(s) { const b = utf8(s); return [7, b.length, ...b]; }
function i32(n) { return [3, n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >>> 24) & 255]; }
function map(o) {
  const entries = Object.entries(o);
  return [13, entries.length, ...entries.flatMap(([k, v]) => [...str(k), ...(typeof v === 'number' ? i32(v) : str(v))])];
}
function invoke(method, args) { return send('momo/window', new Uint8Array([...str(method), ...map(args)]).buffer); }

invoke('setTitle', { view: 0, title: 'Momo · hello_frame' });

// 应用生命周期属于宿主且与窗口无关: 最后一个窗口关闭后应用不会自动退出(只收到 momo/app windowAllClosed)。
// 这个示例只有一个窗口, 关窗即退出(momo/app quit)。
setHandler('momo/app', (data) => {
  const b = new Uint8Array(data);
  const method = new TextDecoder().decode(b.subarray(2, 2 + b[1]));
  if (method === 'windowAllClosed') send('momo/app', new Uint8Array([...str('quit'), 0]).buffer);
  return null;
});

let frameTime = 0;
let frames = 0;
let started = 0;

function starPath(cx, cy, outer, inner, rotation) {
  const path = new Path();
  for (let i = 0; i < 10; i++) {
    const r = i % 2 === 0 ? outer : inner;
    const a = rotation - Math.PI / 2 + (i * Math.PI) / 5;
    const x = cx + r * Math.cos(a);
    const y = cy + r * Math.sin(a);
    if (i === 0) path.moveTo(x, y);
    else path.lineTo(x, y);
  }
  return path.close();
}

function paint(color, setup) {
  const p = new Paint();
  p.color = color;
  if (setup) setup(p);
  return p;
}

function drawScene(width, height, seconds) {
  const recorder = new PictureRecorder();
  const c = recorder.beginRecording(0, 0, width, height);
  c.drawColor(0xfff5f5f7);

  // 渐变卡片
  const card = paint(0xff000000, (p) => {
    p.shader = Gradient.linear(40, 40, 360, 200, [0xff4285f4, 0xff9c27b0, 0xffea4335], [0, 0.5, 1]);
  });
  c.drawRRect(40, 40, 320, 160, 24, card);

  // 卡片上的高光圆(径向渐变)
  const glow = paint(0xffffffff, (p) => {
    p.shader = Gradient.radial(110, 90, 70, [0x66ffffff, 0x00ffffff]);
  });
  c.drawCircle(110, 90, 70, glow);

  // 旋转的星形
  c.drawPath(starPath(520, 120, 80, 32, seconds * 0.8), paint(0xfffbbc05));

  // 描边: 线条与圆
  const stroke = paint(0xff34a853, (p) => {
    p.style = 'stroke';
    p.strokeWidth = 8;
    p.strokeCap = 'round';
  });
  c.drawLine(40, 260, Math.max(60, width - 40), 260, stroke);
  c.drawCircle(120, 360, 50, stroke);

  // 呼吸缩放的方块(save/translate/rotate/scale/restore)
  c.save();
  c.translate(300, 360);
  c.rotate(Math.PI / 4);
  const s = 1 + 0.15 * Math.sin(seconds * 3);
  c.scale(s, s);
  c.drawRect(-40, -40, 80, 80, paint(0xcc9c27b0));
  c.restore();

  return recorder.endRecording();
}

platformDispatcher.onBeginFrame = (frameTimeMs) => {
  frameTime = frameTimeMs;
  if (started === 0) started = frameTimeMs;
};

platformDispatcher.onDrawFrame = () => {
  if (!viewports.has(0)) return;  // 窗口已关闭
  const dpr = view.devicePixelRatio;
  const { width, height } = view.physicalSize;
  const seconds = (frameTime - started) / 1000;
  const picture = drawScene(width / dpr, height / dpr, seconds);

  const builder = new SceneBuilder();
  // 列主序 4x4 缩放矩阵: 逻辑像素 → 物理像素
  builder.pushTransform([dpr, 0, 0, 0, 0, dpr, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  builder.addPicture(0, 0, picture);
  builder.pop();
  view.render(builder.build());

  frames++;
  if (frames % 120 === 0) {
    const fps = (frames * 1000) / Math.max(1, frameTime - started);
    console.log(`hello_frame: ${frames} frames, ~${fps.toFixed(1)} fps @ ${width}x${height}`);
  }
  platformDispatcher.scheduleFrame();  // 持续动画
};

view.onMetricsChanged = () => platformDispatcher.scheduleFrame();

platformDispatcher.scheduleFrame();
