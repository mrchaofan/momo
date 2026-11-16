// 页内查找栏: 工具栏下方的一行(输入框 + 上一个 / 下一个 + 关闭)。回车查找下一个;状态由窗口页面持有。
import {
  Border, BorderRadius, BorderSide, BoxDecoration, Container, EdgeInsets, Expanded, InputDecoration, Row, SizedBox,
  StatelessView, Text, TextField, type BuildContext, type FocusNode, type TextEditingController, type View,
} from "momo:framework";
import { Palette, TextStyles } from "./style";
import { IconTap } from "./tabs";

export const kFindBarHeight = 40;

export class FindBar extends StatelessView {
  constructor(readonly o: {
    controller: TextEditingController;
    focusNode: FocusNode;
    /** 最近一次查找没有匹配。 */
    missing: boolean;
    onChanged: () => void;
    onFind: (backwards: boolean) => void;
    onClose: () => void;
  }) {
    super();
  }

  build(_context: BuildContext): View {
    const { controller, focusNode, missing, onChanged, onFind, onClose } = this.o;
    return new Container({
      height: kFindBarHeight,
      padding: EdgeInsets.symmetric({ horizontal: 10 }),
      decoration: new BoxDecoration({ color: Palette.white, border: new Border({ bottom: new BorderSide({ color: Palette.line, width: 1 }) }) }),
      child: new Row({
        children: [
          new Expanded({ child: new SizedBox({}) }),
          ...(missing ? [new Text("无匹配", { style: TextStyles.status }), new SizedBox({ width: 10 })] : []),
          new SizedBox({
            width: 260,
            child: new TextField({
              controller,
              focusNode,
              style: TextStyles.address,
              onChanged,
              onSubmitted: () => onFind(false),
              decoration: new InputDecoration({
                hintText: "在页面中查找",
                filled: true,
                fillColor: Palette.field,
                borderRadius: BorderRadius.circular(6),
                contentPadding: EdgeInsets.symmetric({ horizontal: 12, vertical: 6 }),
              }),
            }),
          }),
          new SizedBox({ width: 6 }),
          new IconTap({ glyph: "‹", onTap: () => onFind(true) }),
          new IconTap({ glyph: "›", onTap: () => onFind(false) }),
          new IconTap({ glyph: "×", onTap: onClose, style: TextStyles.close }),
        ],
      }),
    });
  }
}
