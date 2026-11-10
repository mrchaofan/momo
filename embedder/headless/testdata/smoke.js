// momo_headless 冒烟测试: 等两帧、画一个红色方块、截图、经 momo/test 通道报告结果。
import { window, PictureRecorder, Paint, SceneBuilder, ParagraphBuilder } from 'momo:ui';
import { send } from 'momo:services';

// 最小 StandardMethodCodec 编码(只用到 string 与 map{passed, failed: int32})。
function str(s) { const b = [...s].map((c) => c.charCodeAt(0)); return [7, b.length, ...b]; }
function i32(n) { return [3, n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >>> 24) & 255]; }
// StandardMethodCodec(Flutter 格式): method 值紧接 args 值。
function call(method, argBytes) {
  return new Uint8Array([...str(method), ...argBytes]).buffer;
}

let frames = 0;
let passed = 0, failed = 0;
const check = (ok, what) => { ok ? passed++ : failed++; send('momo/test', call('log', str((ok ? 'ok   ' : 'FAIL ') + what))); };

window.onBeginFrame = () => { frames++; };
window.onDrawFrame = () => {
  const r = new PictureRecorder();
  const c = r.beginRecording();
  const p = new Paint();
  p.color = 0xFFFF0000;
  c.drawRect(10, 10, 50, 50, p);
  const b = new ParagraphBuilder({ textStyle: { fontSize: 16 } });
  b.addText('headless 文本');
  const para = b.build();
  para.layout(200);
  c.drawParagraph(para, 10, 70);
  const sb = new SceneBuilder();
  sb.addPicture(0, 0, r.endRecording());
  window.render(sb.build());
  if (frames < 3) {
    window.scheduleFrame();
  } else {
    check(window.physicalSize.width === 320, 'physicalSize');
    check(para.height > 0, 'paragraph layout');
    send('momo/test', call('screenshot', [0])).then(() => {
      send('momo/test', call('done', [13, 2, ...str('passed'), ...i32(passed), ...str('failed'), ...i32(failed)]));
    });
  }
};
window.scheduleFrame();
