import {
  AcceleratorModifier, Menu, MenuItem, MethodCall, StandardMethodCodec, Tray, debugOverrideTargetPlatform,
  defaultBinaryMessenger, parseAccelerator,
} from "@momo/framework";
import { expect, group, test } from "./harness";

const codec = StandardMethodCodec.instance;
type Item = Record<string, unknown>;

/** 宿主替身: 记录 JS 发来的调用。 */
function mockHost(channelName: string): { calls: [string, Record<string, unknown>][]; dispose: () => void } {
  const calls: [string, Record<string, unknown>][] = [];
  const replies: Record<string, unknown> = { getBounds: new Float64Array([10, 0, 24, 22]), popup: true };
  defaultBinaryMessenger.setMockMessageHandler(channelName, (data) => {
    const call = codec.decodeMethodCall(data);
    calls.push([call.method, (call.args ?? {}) as Record<string, unknown>]);
    return codec.encodeSuccessEnvelope(replies[call.method] ?? null);
  });
  return { calls, dispose: () => defaultBinaryMessenger.setMockMessageHandler(channelName, null) };
}

async function fromHost(channelName: string, method: string, args: unknown): Promise<void> {
  await defaultBinaryMessenger.handlePlatformMessage(channelName, codec.encodeMethodCall(new MethodCall(method, args)));
}

/** 等待合并的更新(microtask)与通道往返。 */
async function settle(): Promise<void> {
  for (let i = 0; i < 5; i++) await Promise.resolve();
}

group("services › menu", () => {
  test("parseAccelerator", () => {
    const { shift, control, alt, meta } = AcceleratorModifier;
    expect.deepEqual(parseAccelerator("Ctrl+Shift+N"), { key: "n", modifiers: control | shift });
    expect.deepEqual(parseAccelerator("alt+f4"), { key: "F4", modifiers: alt });
    expect.deepEqual(parseAccelerator("Cmd+Plus"), { key: "+", modifiers: meta });
    expect.deepEqual(parseAccelerator("Ctrl++"), { key: "+", modifiers: control });
    expect.deepEqual(parseAccelerator("Return"), { key: "Enter", modifiers: 0 });
    debugOverrideTargetPlatform("macos");
    expect.deepEqual(parseAccelerator("CmdOrCtrl+Q"), { key: "q", modifiers: meta });
    debugOverrideTargetPlatform("windows");
    expect.deepEqual(parseAccelerator("CmdOrCtrl+Q"), { key: "q", modifiers: control });
    debugOverrideTargetPlatform(null);
    for (const bad of ["", "Ctrl+", "Ctrl+A+B", "Ctrl+Nope", "F25"]) expect.throws(() => parseAccelerator(bad));
  });

  test("template: types, roles and structure", () => {
    debugOverrideTargetPlatform("macos");
    const menu = Menu.buildFromTemplate([
      { role: "appMenu" },
      { label: "File", submenu: [{ id: "new", label: "New", accelerator: "Cmd+N" }, { type: "separator" }, { role: "close" }] },
      { role: "editMenu", label: "Bearbeiten" },
    ]);
    debugOverrideTargetPlatform(null);
    expect.deepEqual(menu.items.map((i) => i.type), ["submenu", "submenu", "submenu"]);
    expect.deepEqual(menu.items[0]!.submenu!.items.map((i) => i.role ?? i.type),
      ["about", "separator", "services", "separator", "hide", "hideOthers", "unhide", "separator", "quit"]);
    expect.equal(menu.getMenuItemById("new")!.label, "New");
    expect.isNull(menu.getMenuItemById("missing"));

    const wire = menu._serialize() as Item[];
    const file = wire[1]!["submenu"] as Item[];
    expect.deepEqual(file[0], { id: menu.getMenuItemById("new")!.commandId, type: "normal", label: "New", enabled: true,
                                checked: false, accelerator: { key: "n", modifiers: AcceleratorModifier.meta } });
    expect.equal(file[2]!["role"], "close");
    expect.equal(file[2]!["label"], "", "a role item without a label gets the system's title");
    expect.equal(wire[2]!["label"], "Bearbeiten");

    // 一个菜单项只属于一个菜单;一个菜单只是一个菜单项的子菜单。
    expect.throws(() => Menu.buildFromTemplate([menu.items[0]!]));
    expect.throws(() => new MenuItem({ label: "x", submenu: menu.items[1]!.submenu! }));
    expect.throws(() => new MenuItem({ type: "checkbox", submenu: [] }));
  });

  test("application menu: sent on set, re-sent once per batch of changes, clicks routed to items", async () => {
    const host = mockHost("momo/menu");
    const log: string[] = [];
    const menu = Menu.buildFromTemplate([
      { label: "View", submenu: [
        { id: "wrap", label: "Wrap", type: "checkbox", click: (item) => log.push(`wrap:${item.checked}`) },
        { id: "small", label: "Small", type: "radio", checked: true },
        { id: "large", label: "Large", type: "radio", click: () => log.push("large") },
        { id: "off", label: "Off", enabled: false, click: () => log.push("off") },
        { id: "hidden", label: "Hidden", visible: false },
      ] },
    ]);
    Menu.setApplicationMenu(menu);
    expect.equal(Menu.getApplicationMenu(), menu);
    await settle();
    expect.equal(host.calls.length, 1);
    expect.equal(host.calls[0]![0], "setApplicationMenu");
    const sent = (): Item[] => (host.calls.at(-1)![1]["items"] as Item[])[0]!["submenu"] as Item[];
    expect.deepEqual(sent().map((i) => i["label"]), ["Wrap", "Small", "Large", "Off"], "invisible items are left out");

    // 同一轮里的多次修改合并为一次发送。
    const item = (id: string): MenuItem => menu.getMenuItemById(id)!;
    item("wrap").label = "Word Wrap";
    item("hidden").visible = true;
    item("off").submenu?.append(new MenuItem({ label: "never" }));
    await settle();
    expect.equal(host.calls.length, 2);
    expect.deepEqual(sent().map((i) => i["label"]), ["Word Wrap", "Small", "Large", "Off", "Hidden"]);

    // 宿主报告点击: checkbox 取反、radio 同组互斥, 然后调用 click;不可用的菜单项不响应。
    await fromHost("momo/menu", "itemClicked", { id: item("wrap").commandId });
    await fromHost("momo/menu", "itemClicked", { id: item("large").commandId });
    await fromHost("momo/menu", "itemClicked", { id: item("off").commandId });
    await fromHost("momo/menu", "itemClicked", { id: 123456789 });
    expect.deepEqual(log, ["wrap:true", "large"]);
    expect.isTrue(item("large").checked);
    expect.isFalse(item("small").checked);
    await settle();
    expect.deepEqual(sent().map((i) => i["checked"]), [true, false, true, false, false]);

    // 换掉之后: 不再同步, 点击也不再送达。
    const calls = host.calls.length;
    Menu.setApplicationMenu(null);
    await settle();
    expect.deepEqual(host.calls.at(-1), ["setApplicationMenu", { items: null }]);
    item("wrap").label = "x";
    await fromHost("momo/menu", "itemClicked", { id: item("large").commandId });
    await settle();
    expect.equal(host.calls.length, calls + 1);
    expect.deepEqual(log, ["wrap:true", "large"]);
    host.dispose();
  });

  test("popup resolves when the host reports the menu closed; clicks are routed while it is open", async () => {
    const host = mockHost("momo/menu");
    let clicked = 0;
    const menu = Menu.buildFromTemplate([{ label: "Copy Link", click: () => clicked++ }]);
    let closed = false;
    const done = menu.popup({ x: 12, y: 34 }).then(() => (closed = true));
    await settle();
    expect.equal(host.calls[0]![0], "popup");
    expect.equal(host.calls[0]![1]["x"], 12);
    expect.equal(host.calls[0]![1]["y"], 34);
    await fromHost("momo/menu", "itemClicked", { id: menu.items[0]!.commandId });
    expect.equal(clicked, 1);
    expect.isFalse(closed);
    menu.closePopup();
    await settle();
    expect.equal(host.calls.at(-1)![0], "closePopup");
    expect.isFalse(closed, "the promise waits for the host to report the menu closed");
    await fromHost("momo/menu", "popupClosed", { id: host.calls[0]![1]["id"] });
    await done;
    await fromHost("momo/menu", "itemClicked", { id: menu.items[0]!.commandId });
    expect.equal(clicked, 1, "a closed menu no longer receives clicks");
    host.dispose();
  });

  test("without a host every method is a no-op", async () => {
    const menu = Menu.buildFromTemplate([{ label: "A" }]);
    Menu.setApplicationMenu(menu);
    await menu.popup();
    Menu.setApplicationMenu(null);
    const tray = new Tray("icon.png");
    expect.isNull(await tray.getBounds());
    tray.destroy();
  });
});

group("services › tray", () => {
  test("methods, context menu sync, events and destroy", async () => {
    const host = mockHost("momo/tray");
    const menuHost = mockHost("momo/menu");
    const tray = new Tray("trayTemplate.png");
    tray.setToolTip("Momo");
    tray.setTitle("3");
    tray.setImage("/abs/other.png");
    const log: string[] = [];
    const menu = Menu.buildFromTemplate([{ id: "show", label: "Show", click: () => log.push("show") }]);
    tray.setContextMenu(menu);
    await settle();
    const id = host.calls[0]![1]["id"];
    // 相对路径相对应用的 assets 目录, 绝对路径原样。
    const image = host.calls[0]![1]["image"] as string;
    expect.isTrue(image.endsWith("/assets/trayTemplate.png") && image !== "/assets/trayTemplate.png", image);
    expect.deepEqual(host.calls.slice(0, 4), [
      ["create", { id, image }],
      ["setToolTip", { id, text: "Momo" }],
      ["setTitle", { id, title: "3" }],
      ["setImage", { id, image: "/abs/other.png" }],
    ]);
    expect.equal(host.calls[4]![0], "setContextMenu");

    // 托盘菜单内容变化后重新发送;点击经 momo/menu 送达。
    menu.getMenuItemById("show")!.label = "Show Window";
    await settle();
    expect.equal(((host.calls.at(-1)![1]["items"] as Item[])[0]!)["label"], "Show Window");
    await fromHost("momo/menu", "itemClicked", { id: menu.items[0]!.commandId });

    tray.on("click", (e) => log.push(`click:${e.bounds?.x}`));
    tray.on("right-click", () => log.push("right"));
    tray.on("double-click", () => log.push("double"));
    await fromHost("momo/tray", "click", { id, bounds: new Float64Array([100, 0, 24, 22]) });
    await fromHost("momo/tray", "rightClick", { id, bounds: null });
    await fromHost("momo/tray", "doubleClick", { id });
    await fromHost("momo/tray", "click", { id: 9999 });
    expect.deepEqual(log, ["show", "click:100", "right", "double"]);
    expect.deepEqual(await tray.getBounds(), { x: 10, y: 0, width: 24, height: 22 });

    tray.destroy();
    expect.isTrue(tray.isDestroyed());
    await settle();
    expect.deepEqual(host.calls.slice(-2).map((c) => c[0]), ["setContextMenu", "destroy"]);
    const calls = host.calls.length;
    tray.setToolTip("gone");
    menu.getMenuItemById("show")!.label = "x";
    await fromHost("momo/tray", "click", { id, bounds: null });
    await settle();
    expect.equal(host.calls.length, calls, "a destroyed tray sends nothing");
    expect.equal(log.length, 4);
    host.dispose();
    menuHost.dispose();
  });
});
