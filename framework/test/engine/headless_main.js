// 在真实引擎里运行框架测试(momo_headless --manual-frames)。
// 由 tools/momo-cli 或 CTest 调用: momo_headless --manual-frames --bundle=framework/test/engine/headless_main.js
// 结果经 momo/test 通道(StandardMethodCodec, Flutter 格式)报告给运行器。
import { runTests } from "../../out/test/momo_tests.js";
import { send } from "momo:services";

function utf8(s) {
  const out = [];
  for (const ch of s) {
    let c = ch.codePointAt(0);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
    else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
  }
  return out;
}
function size(n) { return n < 254 ? [n] : n <= 0xffff ? [254, n & 255, n >> 8] : [255, n & 255, (n >> 8) & 255, (n >> 16) & 255, n >>> 24]; }
function str(s) { const b = utf8(s); return [7, ...size(b.length), ...b]; }
function i32(n) { return [3, n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >>> 24) & 255]; }
const call = (method, args) => new Uint8Array([...str(method), ...args]).buffer;

let chain = Promise.resolve();
const log = (line) => { chain = chain.then(() => send("momo/test", call("log", str(line)))); };

runTests({ name: "engine-headless", log, ahemText: false, mock: null }).then((s) => {
  for (const f of s.failures) log(`FAIL ${f.name}: ${f.error}`);
  log(`${s.passed} passed, ${s.failed} failed, ${s.skipped} skipped (engine-headless)`);
  chain.then(() => send("momo/test", call("done",
    [13, 2, ...str("passed"), ...i32(s.passed), ...str("failed"), ...i32(s.failed)])));
}, (e) => {
  log(`runner crashed: ${e && e.stack || e}`);
  chain.then(() => send("momo/test", call("done", [13, 1, ...str("failed"), ...i32(1)])));
});
