// 弹出到单独窗口(类似 Codex 的"在单独窗口中打开"):
//   - DetachedPlaceholder: 原窗口里该标签的占位(页面标题 + "恢复到标签页" / "显示窗口");
//   - DetachedPage: 单独窗口的内容(精简工具栏 + 置顶开关 + 恢复按钮 + 网页)。
// 网页本身是同一个 WebView(GlobalKey), 在两处之间迁移时原生网页视图换窗口, 页面不刷新。
import {
  Align, Alignment, Border, BorderRadius, BorderSide, BoxDecoration, Center, Column, Container, EdgeInsets, Expanded,
  FontWeight, FractionallySizedBox, GestureDetector, MouseRegion, Padding, Row, SizedBox, State, StatefulWidget,
  StatelessWidget, Text, TextStyle, WindowChrome, WindowDragArea, type BuildContext, type Widget,
} from "@momo/framework";
import type { BrowserAppState, DetachedWindow } from "./main";
import { IconButton } from "./icons";
import { Palette, TextStyles, kTabStripHeight, kToolbarHeight, kTrafficLightsWidth } from "./style";
import { IconTap, type Tab } from "./tabs";

/** 胶囊按钮: primary 为黑底白字, 否则浅灰底黑字;active 用于开关的"开"状态(同 primary 外观)。 */
export class Pill extends StatefulWidget {
  constructor(readonly o: { label: string; onTap: () => void; primary?: boolean; small?: boolean }) {
    super();
  }
  createState(): State {
    return new PillState();
  }
}

class PillState extends State<Pill> {
  #hover = false;
  build(_c: BuildContext): Widget {
    const { label, onTap, primary = false, small = false } = this.widget.o;
    const bg = primary ? (this.#hover ? Palette.ink : Palette.black) : (this.#hover ? Palette.line : Palette.hover);
    return new MouseRegion({
      cursor: "click",
      onEnter: () => this.setState(() => (this.#hover = true)),
      onExit: () => this.setState(() => (this.#hover = false)),
      child: new GestureDetector({
        behavior: "opaque",
        onTap,
        child: new Container({
          height: small ? 28 : 34,
          padding: EdgeInsets.symmetric({ horizontal: small ? 12 : 16 }),
          decoration: new BoxDecoration({ color: bg, borderRadius: BorderRadius.circular(small ? 14 : 17) }),
          child: new Center({
            child: new Text(label, {
              style: new TextStyle({ fontSize: small ? 12.5 : 14, color: primary ? Palette.white : Palette.black, fontWeight: FontWeight.w500 }),
            }),
          }),
        }),
      }),
    });
  }
}

/** 页面标题的首字母徽标(没有 favicon 时的占位图标)。 */
function badge(title: string): Widget {
  const ch = (title.trim()[0] ?? "·").toUpperCase();
  return new Container({
    width: 64,
    height: 64,
    decoration: new BoxDecoration({
      color: Palette.white,
      border: new Border({
        top: new BorderSide({ color: Palette.line }), right: new BorderSide({ color: Palette.line }),
        bottom: new BorderSide({ color: Palette.line }), left: new BorderSide({ color: Palette.line }),
      }),
      borderRadius: BorderRadius.circular(16),
    }),
    child: new Center({ child: new Text(ch, { style: new TextStyle({ fontSize: 28, color: Palette.black, fontWeight: FontWeight.w600 }) }) }),
  });
}

/** 原窗口里已弹出标签的占位。 */
export class DetachedPlaceholder extends StatelessWidget {
  constructor(readonly o: { app: BrowserAppState; tab: Tab }) {
    super();
  }
  build(_c: BuildContext): Widget {
    const { app, tab } = this.o;
    return new Container({
      color: Palette.white,
      child: new Center({
        child: new Padding({
          padding: EdgeInsets.symmetric({ horizontal: 48 }),
          child: new Column({
            mainAxisSize: "min",
            children: [
              badge(tab.displayTitle),
              new SizedBox({ height: 20 }),
              new Text(tab.displayTitle, {
                textAlign: "center",
                maxLines: 3,
                overflow: "ellipsis",
                style: new TextStyle({ fontSize: 18, color: Palette.black }),
              }),
              new SizedBox({ height: 10 }),
              new Text("此标签页已在单独窗口中打开", { style: new TextStyle({ fontSize: 13, color: Palette.muted }) }),
              new SizedBox({ height: 20 }),
              new Row({
                mainAxisSize: "min",
                children: [
                  new Pill({ label: "恢复到标签页", primary: true, onTap: () => app.restoreTab(tab) }),
                  new SizedBox({ width: 10 }),
                  new Pill({ label: "显示窗口", onTap: () => app.showDetached(tab) }),
                ],
              }),
            ],
          }),
        }),
      }),
    });
  }
}

/**
 * 单独窗口的内容(无边框窗口): 顶栏(红绿灯、网页标题、置顶与恢复图标按钮)+ 工具栏(后退/前进/刷新、地址)
 * + 加载进度 + 网页。顶栏除按钮外都可拖动窗口。
 */
export class DetachedPage extends StatelessWidget {
  constructor(readonly o: { app: BrowserAppState; detached: DetachedWindow; page: Widget }) {
    super();
  }
  build(_c: BuildContext): Widget {
    const { app, detached: d, page } = this.o;
    const tab = d.tab;
    const web = tab.controller;
    let host = tab.url;
    const m = /^[a-z]+:\/\/([^/?#]+)/i.exec(tab.url);
    if (m) host = m[1]!;
    if (tab.url === "about:blank" || tab.url === "" || tab.url.startsWith("momo:")) host = tab.displayTitle;
    const progress = tab.loading ? Math.max(0.05, tab.progress) : 0;
    const titleBar = new Container({
      height: kTabStripHeight,
      padding: EdgeInsets.only({ right: 8 }),
      child: new Row({
        children: [
          new SizedBox({ width: kTrafficLightsWidth, height: kTabStripHeight, child: new WindowDragArea() }),
          new Expanded({
            child: new WindowDragArea({
              child: new Align({
                alignment: Alignment.centerLeft,
                child: new Text(tab.displayTitle, { maxLines: 1, overflow: "ellipsis", style: TextStyles.tabActive }),
              }),
            }),
          }),
          new SizedBox({ width: 12 }),
          new IconButton({ icon: "pin", active: d.pinned, onTap: () => app.togglePin(d) }),
          new SizedBox({ width: 4 }),
          new IconButton({ icon: "restore", onTap: () => app.restoreTab(tab) }),
        ],
      }),
    });
    return new WindowChrome({ trafficLightsCenterY: kTabStripHeight / 2, child: new Container({
      color: Palette.white,
      child: new Column({
        children: [
          titleBar,
          new Container({
            height: kToolbarHeight,
            padding: EdgeInsets.symmetric({ horizontal: 10 }),
            decoration: new BoxDecoration({ color: Palette.white, border: new Border({ bottom: new BorderSide({ color: Palette.line, width: 1 }) }) }),
            child: new Row({
              children: [
                new IconTap({ glyph: "‹", onTap: web && tab.canGoBack ? () => void web.goBack() : null }),
                new IconTap({ glyph: "›", onTap: web && tab.canGoForward ? () => void web.goForward() : null }),
                new IconTap({ glyph: tab.loading ? "×" : "↻", onTap: web ? () => void (tab.loading ? web.stop() : web.reload()) : null }),
                new SizedBox({ width: 10 }),
                new Expanded({
                  child: new Container({
                    height: 30,
                    decoration: new BoxDecoration({ color: Palette.field, borderRadius: BorderRadius.circular(6) }),
                    child: new Center({ child: new Text(host, { maxLines: 1, overflow: "ellipsis", style: TextStyles.address }) }),
                  }),
                }),
                new SizedBox({ width: 6 }),
              ],
            }),
          }),
          new SizedBox({
            height: 2,
            child: new Align({
              alignment: Alignment.centerLeft,
              child: new FractionallySizedBox({ widthFactor: progress, child: new Container({ color: progress > 0 ? Palette.black : Palette.white }) }),
            }),
          }),
          new Expanded({ child: page }),
        ],
      }),
    }) });
  }
}
