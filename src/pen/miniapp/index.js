import global from "global";
import fs from "fs";

const native = new global.Global();
const APPID = "__APPID__";
const CACHE = "/tmp/pendesk-" + APPID;
const RUN =
  "cache=" + CACHE + "; if [ -r \"$cache\" ]; then read -r app < \"$cache\"; [ -f \"$app\" ] && exec /bin/sh \"$app\" \"$@\"; fi; latest=; for app in /userdisk/*/data/mini_app/pkg/" + APPID + "/*/bin/run /userdisk/*/*/data/mini_app/pkg/" + APPID + "/*/bin/run /userdata/*/data/mini_app/pkg/" + APPID + "/*/bin/run /userdata/*/*/data/mini_app/pkg/" + APPID + "/*/bin/run; do [ -f \"$app\" ] || continue; if [ -z \"$latest\" ] || [ \"$app\" -nt \"$latest\" ]; then latest=$app; fi; done; [ -n \"$latest\" ] && { printf '%s\\n' \"$latest\" > \"$cache\"; exec /bin/sh \"$latest\" \"$@\"; }; exit 1";
const quote = value => "'" + value.replace(/'/g, "'\\''") + "'";
const command = "/bin/setsid /bin/sh -lc " + quote("rm -f " + CACHE + "; " + RUN);
const STATE_BLOCKED = 1;
const STATE_VIDEO_PAUSED = 2;
const FONT = "Google Sans Flex";
const icons = {
  keyboard: "kb.png", folder: "folder.png", file: "file.png", back: "back.png", sound: "sound.png"
};

function control(bytes) {
  const packet = bytes.map(value => ("0" + (value & 255).toString(16)).slice(-2)).join("");
  native.execShell("/bin/sh -lc " + quote(RUN) + " -- input " + packet + " >/dev/null 2>&1 &");
}

function launch() {
  native.execShell(command + " < /dev/null > /dev/null 2>&1 &");
}

function state(blocked, paused) {
  control([2, (blocked ? STATE_BLOCKED : 0) | (paused ? STATE_VIDEO_PAUSED : 0)]);
}

const keys = {
  Esc: 27, Tab: 9, Back: 8, Enter: 13, Del: 46, End: 35, " ": 32,
  Left: 37, Up: 38, Right: 39, Down: 40,
  "`": 192, "-": 189, "=": 187, "[": 219, "]": 221, "\\": 220,
  ";": 186, "'": 222, ",": 188, ".": 190, "/": 191
};
const shifted = {
  "`": "~", "1": "!", "2": "@", "3": "#", "4": "$", "5": "%", "6": "^",
  "7": "&", "8": "*", "9": "(", "0": ")", "-": "_", "=": "+",
  "[": "{", "]": "}", "\\": "|", ";": ":", "'": "\"", ",": "<", ".": ">", "/": "?"
};
const arrows = { Left: "\u2190", Up: "\u2191", Right: "\u2192", Down: "\u2193" };
const keyboardWidth = 15;
const fileRows = 8;
const fileVisible = 7;
const padX = 65535 / 936;
const padY = 65535 / 256;
const key = (label, span = 1) => ({ label, span });
const close = (span = 1) => ({ close: true, span });
const gap = (span = 1) => ({ span });
const short = value => Math.max(-32768, Math.min(32767, Math.round(value)));
const move = (x, y) => [0x20, short(x) >> 8, short(x), short(y) >> 8, short(y)];
const button = (value, down) => [0x21, value, down ? 1 : 0];
const wheel = value => [0x26, short(value) >> 8, short(value)];
const row = (...entries) => {
  let column = 0;
  return entries.reduce((placed, entry) => {
    const item = typeof entry === "string" ? key(entry) : entry;
    if (item.label || item.close) placed.push({ label: item.label, close: item.close, column, span: item.span });
    column += item.span;
    return placed;
  }, []);
};
const keyRows = [
  row("Esc", "F1", "F2", "F3", "F4", "F5", "F6", close(), "F7", "F8", "F9", "F10", "F11", "F12", "Del"),
  row("`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", key("Back", 2)),
  row(key("Tab", 1.5), "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", key("\\", 1.5)),
  row(key("Caps", 1.75), "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", key("Enter", 2.25)),
  row(key("Shift", 2.25), "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", gap(0.75), "Up", gap()),
  row(key("Ctrl", 1.25), key("Win", 1.25), key("Alt", 1.25), key(" ", 6.25), key("End", 2), "Left", "Down", "Right")
];
const modifierCodes = { Shift: 16, Ctrl: 17, Win: 91, Alt: 18 };

const script = {
  data() {
    try {
      launch();
      state(false, false);
    } catch {}
    return {
      active: true, ready: false, frame: Date.now(), job: null, guard: null, view: "desktop", pad: null, held: [], fileTouch: null, fileScrolled: false, audioOn: false,
      fileState: { pen: [], windows: [], transfer: 0, progress: 0 }, fileOffset: { pen: 0, windows: 0 }, fileWatch: null,
      caps: false, modifiers: { Shift: false, Ctrl: false, Win: false, Alt: false }
    };
  },
  activated() {
    const resumed = !this.active;
    this.active = true;
    if (!resumed) return this.requestFrame();
    launch();
    this.view = "desktop";
    state(false, false);
    this.requestFrame();
  },
  deactivated() {
    this.suspend();
  },
  beforeDestroy() {
    this.suspend();
  },
  methods: {
    schedule(failed) {
      if (this.guard !== null) { clearTimeout(this.guard); this.guard = null; }
      if (!this.active || (this.view !== "desktop" && this.view !== "keyboard") || this.job !== null) return;
      if (failed) {
        this.ready = false;
        state(this.view === "keyboard", false);
      } else if (!this.ready) {
        state(this.view === "keyboard", false);
        this.ready = true;
      }
      this.job = setTimeout(() => {
        this.job = null;
        this.requestFrame();
      }, failed ? 500 : 0);
    },
    requestFrame() {
      if (!this.active || (this.view !== "desktop" && this.view !== "keyboard")) return;
      this.frame++;
      if (this.guard !== null) clearTimeout(this.guard);
      this.guard = setTimeout(() => { this.guard = null; this.requestFrame(); }, 900);
    },
    cancelFrame() {
      if (this.job !== null) clearTimeout(this.job);
      if (this.guard !== null) clearTimeout(this.guard);
      this.job = null;
      this.guard = null;
    },
    suspend() {
      if (!this.active) return;
      this.active = false;
      this.ready = false;
      this.view = "desktop";
      this.audioOn = false;
      this.cancelFrame();
      this.finishPad(false);
      this.releaseKeys();
      state(true, true);
    },
    toggleAudio() {
      this.audioOn = !this.audioOn;
      control([0x27, this.audioOn ? 1 : 0]);
    },
    releaseKeys() {
      const packet = [];
      this.held.forEach(held => { if (held.timer !== null) clearTimeout(held.timer); });
      Object.keys(modifierCodes).forEach(label => {
        if (this.modifiers[label]) packet.push(0x23, 0, modifierCodes[label], 0);
      });
      if (packet.length) control(packet);
      Object.keys(modifierCodes).forEach(label => { this.modifiers[label] = false; });
      this.caps = false;
      this.held = [];
    },
    showTools() {
      this.finishPad(false);
      this.releaseKeys();
      this.cancelFrame();
      this.view = "tools";
      state(true, true);
    },
    showKeyboard() {
      this.view = "keyboard";
      this.frame++;
      state(true, false);
    },
    showFiles() {
      this.view = "files";
      this.fileOffset.pen = this.fileOffset.windows = 0;
      this.fileState = { pen: [], windows: [], transfer: 0, progress: 0 };
      state(true, true);
      this.fileAction("reset");
    },
    showDesktop() {
      this.finishPad(false);
      this.releaseKeys();
      this.view = "desktop";
      state(false, false);
      this.requestFrame();
    },
    fileAction(action) {
      if (action.indexOf("open/pen/") === 0) this.fileOffset.pen = 0;
      if (action.indexOf("open/windows/") === 0) this.fileOffset.windows = 0;
      if (action.indexOf("transfer/") === 0) {
        const side = action === "transfer/push" ? "pen" : "windows";
        const selected = this.fileState[side].filter(entry => entry.state === 1).map(entry => entry.name);
        if (!selected.length) return;
        this.fileWatch = { transfer: side === "pen" ? 1 : 2, side, selected, started: false };
        native.execShell("/bin/sh -lc " + quote(RUN) + " -- files " + action);
        return this.watchFiles();
      }
      native.execShell("/bin/sh -lc " + quote(RUN) + " -- files " + action);
      [200, 1200].forEach(delay => setTimeout(() => {
        if (this.view === "files") this.loadFiles();
      }, delay));
    },
    watchFiles() {
      this.loadFiles().then(() => {
        const watch = this.fileWatch;
        if (!watch) return;
        if (this.fileState.transfer === watch.transfer) watch.started = true;
        else if (watch.started || watch.selected.some(name => this.fileState[watch.side].some(entry => entry.name === name && entry.state > 1))) this.fileWatch = null;
        if (this.view === "files" && this.fileWatch) setTimeout(() => this.watchFiles(), 100);
      });
    },
    loadFiles() {
      return fs.readFile("/tmp/pendesk-files.json", "utf8").then(text => {
        const fileState = JSON.parse(typeof text === "string" ? text : String(text));
        if (Array.isArray(fileState.pen) && Array.isArray(fileState.windows)) {
          this.fileState = fileState;
          this.fileScroll("pen", this.fileOffset.pen);
          this.fileScroll("windows", this.fileOffset.windows);
        }
      }).catch(() => {});
    },
    fileY(event) {
      const touch = event && ((event.changedTouches && event.changedTouches[0]) || (event.touches && event.touches[0]) || event);
      return touch && (touch.pageY || touch.clientY || touch.y || touch.screenY || 0);
    },
    fileTouchStart(side, event) {
      this.fileScrolled = false;
      this.fileTouch = { side, y: this.fileY(event), offset: this.fileOffset[side] };
    },
    fileScroll(side, offset) {
      this.fileOffset[side] = Math.max(0, Math.min(Math.max(0, this.fileState[side].length - fileVisible), offset));
    },
    fileTouchMove(side, event) {
      const touch = this.fileTouch;
      if (!touch || touch.side !== side) return;
      const distance = touch.y - this.fileY(event);
      if (Math.abs(distance) < 6) return;
      this.fileScrolled = true;
      this.fileScroll(side, touch.offset + Math.round(distance / 27));
    },
    fileTouchEnd(side, event) {
      const touch = this.fileTouch;
      this.fileTouch = null;
      if (!touch || touch.side !== side) return;
      const distance = touch.y - this.fileY(event);
      if (Math.abs(distance) >= 6) {
        this.fileScrolled = true;
        this.fileScroll(side, touch.offset + Math.round(distance / 27));
        setTimeout(() => { this.fileScrolled = false; }, 80);
      }
    },
    padTouches(event) {
      const active = event && event.touches;
      const changed = event && event.changedTouches;
      return active && active.length ? active : changed && changed.length ? changed : event ? [event] : [];
    },
    padPosition(touch) {
      return {
        id: touch.identifier === undefined ? 0 : touch.identifier,
        x: touch.pageX === undefined ? (touch.clientX === undefined ? touch.x || 0 : touch.clientX) : touch.pageX,
        y: touch.pageY === undefined ? (touch.clientY === undefined ? touch.y || 0 : touch.clientY) : touch.pageY
      };
    },
    padPoint(touches, id) {
      for (let index = 0; index < touches.length; index++) {
        const point = this.padPosition(touches[index]);
        if (point.id === id) return point;
      }
      return null;
    },
    padFocus(touches) {
      let x = 0;
      let y = 0;
      for (let index = 0; index < touches.length; index++) {
        const point = this.padPosition(touches[index]);
        x += point.x;
        y += point.y;
      }
      return { x: x / touches.length, y: y / touches.length };
    },
    padStart(event) {
      const touches = this.padTouches(event);
      const changed = event && event.changedTouches || touches;
      if (!changed.length) return;
      if (this.pad) {
        const pad = this.pad;
        pad.max = Math.max(pad.max, touches.length);
        if (touches.length > 1) {
          const point = this.padFocus(touches);
          pad.x = pad.originX = point.x;
          pad.y = pad.originY = point.y;
          if (pad.press !== null) clearTimeout(pad.press);
          pad.press = null;
        }
        return;
      }
      const point = this.padPosition(changed[0]);
      const pad = { id: point.id, x: point.x, y: point.y, originX: point.x, originY: point.y, max: touches.length || 1, moved: false, dragging: false, dx: 0, dy: 0, wheel: 0, press: null, timer: null };
      this.pad = pad;
      pad.press = setTimeout(() => {
        if (this.pad === pad && !pad.moved && pad.max === 1) {
          pad.dragging = true;
          control(button(1, true));
        }
      }, 400);
    },
    padQueue(pad) {
      if (pad.timer !== null) return;
      pad.timer = setTimeout(() => {
        pad.timer = null;
        if (this.pad !== pad) return;
        const packet = this.padPacket(pad);
        if (packet.length) control(packet);
      }, 16);
    },
    padPacket(pad) {
      const dx = pad.dx;
      const dy = pad.dy;
      const scroll = pad.wheel;
      pad.dx = pad.dy = pad.wheel = 0;
      const packet = dx || dy ? move(dy * padY, -dx * padX) : [];
      if (scroll) packet.push(...wheel(scroll));
      return packet;
    },
    padMove(event) {
      const pad = this.pad;
      const touches = this.padTouches(event);
      if (!pad || !touches.length) return;
      pad.max = Math.max(pad.max, touches.length);
      const point = touches.length > 1 ? this.padFocus(touches) : this.padPoint(touches, pad.id);
      if (!point) return;
      const dx = point.x - pad.x;
      const dy = point.y - pad.y;
      pad.x = point.x;
      pad.y = point.y;
      if (!dx && !dy) return;
      if (!pad.moved && Math.abs(point.x - pad.originX) + Math.abs(point.y - pad.originY) > 4) {
        pad.moved = true;
        if (pad.press !== null) clearTimeout(pad.press);
        pad.press = null;
      }
      if (touches.length > 1) {
        pad.wheel += dy * 8;
        this.padQueue(pad);
      } else {
        pad.dx += dx;
        pad.dy += dy;
        this.padQueue(pad);
      }
    },
    finishPad(click) {
      const pad = this.pad;
      if (!pad) return;
      this.pad = null;
      if (pad.press !== null) clearTimeout(pad.press);
      if (pad.timer !== null) clearTimeout(pad.timer);
      const packet = this.padPacket(pad);
      if (pad.dragging) packet.push(...button(1, false));
      else if (click && !pad.moved) packet.push(...button(pad.max > 1 ? 2 : 1, true), ...button(pad.max > 1 ? 2 : 1, false));
      if (packet.length) control(packet);
    },
    padEnd(event) {
      const pad = this.pad;
      if (!pad) return;
      const touches = event && event.touches || [];
      const point = this.padPoint(touches, pad.id);
      if (point) {
        pad.x = point.x;
        pad.y = point.y;
        return;
      }
      this.finishPad(true);
    },
    keyPoints(event) {
      const changed = event && event.changedTouches;
      return changed && changed.length ? changed : event ? [event] : [];
    },
    tap(label) {
      let code;
      if (/^F\d+$/.test(label)) {
        code = 111 + Number(label.slice(1));
      } else {
        code = keys[label] || label.charCodeAt(0);
      }
      const shift = this.caps && /^[A-Z]$/.test(label) && !this.modifiers.Shift;
      const packet = shift ? [0x23, 0, 16, 1] : [];
      packet.push(0x23, 0, code, 1, 0x23, 0, code, 0);
      if (shift) packet.push(0x23, 0, 16, 0);
      control(packet);
    },
    keyStart(entry, event) {
      this.keyPoints(event).forEach(point => {
        const id = this.padPosition(point).id;
        if (this.held.some(held => held.id === id)) return;
        const held = { id, entry, timer: null };
        this.held.push(held);
        const label = entry.label;
        if (!label) return;
        if (modifierCodes[label]) {
          this.modifiers[label] = !this.modifiers[label];
          control([0x23, 0, modifierCodes[label], this.modifiers[label] ? 1 : 0]);
          return;
        }
        if (label === "Caps") {
          this.caps = !this.caps;
          return;
        }
        this.tap(label);
        const repeat = () => {
          if (this.held.indexOf(held) < 0) return;
          this.tap(label);
          held.timer = setTimeout(repeat, 30);
        };
        held.timer = setTimeout(repeat, 90);
      });
    },
    keyEnd(event) {
      this.keyPoints(event).forEach(point => {
        const id = this.padPosition(point).id;
        const index = this.held.findIndex(held => held.id === id);
        if (index < 0) return;
        const held = this.held[index];
        this.held.splice(index, 1);
        if (held.timer !== null) clearTimeout(held.timer);
        if (held.entry.close) return this.showTools();
      });
    }
  }
};

const style = {
  "_": {
    root: { width: "100%", height: "100%", position: "relative" },
    font: { fontFamily: FONT },
    desktop: { width: "100%", height: "100%", position: "absolute", top: 0, left: 0 },
    toolToggle: { position: "absolute", bottom: 8, right: 8, width: 27, height: 27, borderRadius: 14, backgroundColor: "#0078d4" },
    tools: { width: "100%", height: "100%", position: "relative" },
    toolItem: { position: "absolute", top: 9, width: 162, height: 27, flexDirection: "row", alignItems: "center", backgroundColor: "#191B21", borderRadius: 3 },
    toolKeyboard: { left: 72 },
    toolSound: { left: 246 },
    toolFiles: { left: 420 },
    toolText: { color: "#d9e1e8", fontSize: 18, lineHeight: "27px" },
    toolTextOn: { color: "#a8c7fa" },
    toolBack: { position: "absolute", top: 9, left: 9, width: 27, height: 27, alignItems: "center", justifyContent: "center" },
    toolBackIcon: { width: 27, height: 27 },
    toolItemIcon: { width: 27, height: 27, marginLeft: 8, marginRight: 12 },
    keyboard: { position: "absolute", bottom: 0, left: 0, width: "100%", height: "100%" },
    keyArea: { position: "absolute", top: 0, left: 126, width: 684, height: "100%", flexDirection: "column" },
    touchPad: { position: "absolute", top: 0, width: 126, height: "100%", backgroundColor: "transparent" },
    touchPadLeft: { left: 0 },
    touchPadRight: { left: 810 },
    keyRow: { flex: 1, position: "relative" },
    button: { position: "absolute", top: 0, height: "100%", margin: 0, alignItems: "center", justifyContent: "center" },
    label: { color: "#77767b", opacity: 0.6, fontSize: 18, textAlign: "center" },
    selectedLabel: { color: "#a8c7fa", opacity: 1 },
    files: { width: "100%", height: "100%", position: "relative" },
    fileDevice: { position: "absolute", top: 9, width: 450, color: "#f4f6f8", fontSize: 18, textAlign: "center" },
    filePenTitle: { left: 16 },
    fileWindowsTitle: { left: 469 },
    fileList: { position: "absolute", top: 36, bottom: 3, width: 450, backgroundColor: "#252a32", borderRadius: 3, overflow: "hidden" },
    fileEntries: { position: "absolute", top: 0, left: 0, width: 450, bottom: 27, overflow: "hidden" },
    filePen: { left: 16 },
    fileWindows: { left: 469 },
    fileRow: { position: "absolute", left: 0, width: 450, height: 27 },
    fileEntryMain: { position: "absolute", top: 0, left: 0, width: 450, height: 27 },
    fileEntryIconButton: { position: "absolute", top: 0, left: 6, width: 27, height: 27, alignItems: "center", justifyContent: "center" },
    fileEntryIcon: { width: 27, height: 27 },
    fileName: { position: "absolute", top: 0, left: 42, height: 27, color: "#f0f3f7", fontSize: 18, lineHeight: "27px" },
    fileSelected: { color: "#0078d4" },
    fileSuccess: { color: "#107c10" },
    fileFailed: { color: "#d13438" },
    fileSize: { position: "absolute", top: 0, left: 388, width: 54, height: 27, color: "#8993a0", fontSize: 18, lineHeight: "27px", textAlign: "right" },
    fileTransfer: { position: "absolute", bottom: 0, left: 0, width: 450, height: 27, overflow: "hidden", flexDirection: "row", alignItems: "center", justifyContent: "center" },
    fileProgress: { position: "absolute", top: 0, left: 0, height: "100%", backgroundColor: "#0078d4" },
    filePush: { backgroundColor: "#173a58" },
    filePull: { backgroundColor: "#173f49" },
    fileTransferText: { color: "#f4f6f8", fontSize: 18 }
  }
};

const render = function () {
  const create = this.$createElement;
  const text = (value, classes, style) => create("text", {
    staticClass: ["font"].concat(classes), style, attrs: { value }
  });
  const remoteFrame = classes => create("image", {
    staticClass: classes,
    attrs: { src: "http://127.0.0.1:999/frame?" + this.frame },
    on: { load: event => this.schedule(!!event && event.success === false), error: () => this.schedule(true) }
  });
  const icon = (value, classes) => create("div", {
    staticClass: classes,
    style: { backgroundImage: "url(" + value + ")", backgroundSize: "contain", backgroundRepeat: "no-repeat", backgroundPosition: "center" }
  });
  const keyButton = entry => {
    var value = entry.label || "";
    var shown = this.modifiers.Shift && shifted[value] ? shifted[value] : arrows[value] || value;
    var selected = modifierCodes[value] ? this.modifiers[value] : value === "Caps" ? this.caps : this.held.some(item => item.entry === entry);
    return create("div", {
      staticClass: ["button"],
      style: { left: entry.column / keyboardWidth * 100 + "%", width: entry.span / keyboardWidth * 100 + "%" },
      on: { touchstart: event => this.keyStart(entry, event), touchend: event => this.keyEnd(event), touchcancel: () => this.releaseKeys() }
    }, shown ? [text(shown, ["label"].concat(selected ? ["selectedLabel"] : []))] : []);
  };
  const toolBack = action => create("div", { staticClass: ["toolBack"], on: { click: action } }, [
    icon(icons.back, ["toolBackIcon"])
  ]);
  const shorten = (value, length) => value.length > length ? value.slice(0, length - 2) + ".." : value;
  const size = value => {
    const units = ["B", "KB", "MB", "GB"];
    let unit = 0;
    while (value >= 1024 && unit + 1 < units.length) {
      value = Math.floor(value / 1024);
      unit++;
    }
    return value + units[unit];
  };
  if (this.view === "desktop") {
    return create("div", { staticClass: ["root"] }, [
      remoteFrame(["desktop"]),
      create("div", { staticClass: ["toolToggle"], on: { click: () => this.showTools() } })
    ]);
  }
  if (this.view === "tools") {
    return create("div", { staticClass: ["tools"] }, [
      create("div", { staticClass: ["toolItem", "toolKeyboard"], on: { click: () => this.showKeyboard() } }, [
        icon(icons.keyboard, ["toolItemIcon"]),
        text("键盘", ["toolText"])
      ]),
      create("div", { staticClass: ["toolItem", "toolSound"], on: { click: () => this.toggleAudio() } }, [
        icon(icons.sound, ["toolItemIcon"]),
        text("声音", ["toolText"].concat(this.audioOn ? ["toolTextOn"] : []))
      ]),
      create("div", { staticClass: ["toolItem", "toolFiles"], on: { click: () => this.showFiles() } }, [
        icon(icons.folder, ["toolItemIcon"]),
        text("文件", ["toolText"])
      ]),
      toolBack(() => this.showDesktop())
    ]);
  }
  if (this.view === "keyboard") {
    return create("div", { staticClass: ["root"] }, [
      remoteFrame(["desktop"]),
      create("div", { staticClass: ["keyboard"] }, [
        create("div", { staticClass: ["touchPad", "touchPadLeft"], on: { touchstart: event => this.padStart(event), touchmove: event => this.padMove(event), touchend: event => this.padEnd(event), touchcancel: () => this.finishPad(false) } }),
        create("div", { staticClass: ["keyArea"] }, keyRows.map(row =>
          create("div", { staticClass: ["keyRow"] }, row.map(keyButton))
        )),
        create("div", { staticClass: ["touchPad", "touchPadRight"], on: { touchstart: event => this.padStart(event), touchmove: event => this.padMove(event), touchend: event => this.padEnd(event), touchcancel: () => this.finishPad(false) } })
      ])
    ]);
  }
  const fileRow = (side, entry, index) => {
    var state = entry.state === 1 ? "fileSelected" : entry.state === 2 ? "fileSuccess" : entry.state === 3 ? "fileFailed" : "";
    var primary = "open/" + side + "/" + index;
    var iconAction = "select/" + side + "/" + index;
    if (!entry.parent && !entry.directory) primary = iconAction;
    if (entry.parent) iconAction = primary;
    var contents = [
      text(shorten(entry.name, entry.directory ? 27 : 20), ["fileName"].concat(state ? [state] : []))
    ];
    if (!entry.directory) contents.push(text(size(entry.size), ["fileSize"].concat(state ? [state] : [])));
    return create("div", {
      staticClass: ["fileRow"], style: { top: (index - this.fileOffset[side]) * 27 }
    }, [
      create("div", { staticClass: ["fileEntryMain"], on: { click: () => !this.fileScrolled && this.fileAction(primary) } }, contents),
      create("div", { staticClass: ["fileEntryIconButton"], on: { click: () => !this.fileScrolled && this.fileAction(iconAction) } }, [
        icon(entry.parent || entry.directory ? icons.folder : icons.file, ["fileEntryIcon"])
      ])
    ]);
  };
  const transfer = (side, action, classes, label) => {
    const current = side === "pen" ? 1 : 2;
    const progress = this.fileState.transfer === current ? this.fileState.progress : this.fileWatch && this.fileWatch.transfer === current ? 0 : -1;
    return create("div", {
      staticClass: ["fileTransfer"].concat(classes), on: { click: () => progress < 0 && this.fileAction(action) }
    }, progress < 0 ? [text(label, ["fileTransferText"])] : [
      create("div", { staticClass: ["fileProgress"], style: { width: progress + "%" } }),
      text(progress + "%", ["fileTransferText"])
    ]);
  };
  const list = (side, entries, action, classes, label) => {
    const offset = this.fileOffset[side];
    return create("div", {
      staticClass: ["fileList", side === "pen" ? "filePen" : "fileWindows"],
      on: {
        touchstart: event => this.fileTouchStart(side, event),
        touchmove: event => this.fileTouchMove(side, event),
        touchend: event => this.fileTouchEnd(side, event)
      }
    }, [
      create("div", { staticClass: ["fileEntries"] }, entries.slice(offset, offset + fileRows).map((entry, index) => fileRow(side, entry, offset + index))),
      transfer(side, action, classes, label)
    ]);
  };
  return create("div", { staticClass: ["files"] }, [
    toolBack(() => this.showTools()),
    text("\u672c\u5730", ["fileDevice", "filePenTitle"]),
    text("\u8fdc\u7aef", ["fileDevice", "fileWindowsTitle"]),
    list("pen", this.fileState.pen, "transfer/push", ["filePush"], "\u4f20\u8fdc\u7aef"),
    list("windows", this.fileState.windows, "transfer/pull", ["filePull"], "\u4f20\u672c\u5730")
  ]);
};

script.render = render;
script.staticRenderFns = [];
script._compiled = true;
script.style = style._;
script.themes = style;

export default script;
