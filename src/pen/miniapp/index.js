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
const STATE_MOUSE_PAUSED = 4;
const STATE_RECORDING = 8;
const FONT = "Google Sans Flex";
const icons = {
  screen: "view.png", mouse: "mouse.png", touch: "touch.png", keyboard: "kb.png", mic: "mic.png", camera: "cam.png", folder: "folder.png", file: "file.png", back: "back.png", stop: "stop.png", sound: "sound.png", power: "power.png"
};

function control(bytes, ordered = false) {
  const packet = bytes.map(value => "\\" + ("00" + (value & 255).toString(8)).slice(-3)).join("");
  const command = "/bin/sh -lc " + quote(RUN) + " -- input " + quote(packet) + " >/dev/null 2>&1";
  native.execShell(ordered ? command : command + " &");
}

function launch() {
  native.execShell(command + " < /dev/null > /dev/null 2>&1 &");
}

function state(blocked, paused, mousePaused = false, recording = false) {
  control([2, (blocked ? STATE_BLOCKED : 0) | (paused ? STATE_VIDEO_PAUSED : 0) | (mousePaused ? STATE_MOUSE_PAUSED : 0) | (recording ? STATE_RECORDING : 0)], true);
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
const fileRows = 7;
const gestureThreshold = 9;
const mouseSensitivityX = 70;
const mouseSensitivityY = 256;
const touchDistance = (x, y) => Math.floor(Math.abs(x) > Math.abs(y) ? Math.abs(x) + Math.abs(y) / 2 : Math.abs(y) + Math.abs(x) / 2);
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
      state(false, true, true, false);
    } catch {}
    return {
      active: true, ready: false, frame: Date.now(), job: null, guard: null, view: "desktop", pad: null, held: [], directTouches: {}, fileTouch: null, fileScrolled: false, audioOn: false, screenOn: false, mouseOn: false, touchOn: false, micOn: false, cameraOn: false,
      fileState: { pen: [], windows: [], revision: 0 }, fileOffset: { pen: 0, windows: 0 }, fileSession: 0,
      caps: false, modifiers: { Shift: false, Ctrl: false, Win: false, Alt: false }
    };
  },
  activated() {
    const resumed = !this.active;
    this.active = true;
    if (!resumed) return this.requestFrame();
    launch();
    this.view = "desktop";
    state(false, !this.screenOn, !this.mouseOn, this.micOn);
    this.requestFrame();
  },
  beforeDestroy() {
    this.stop();
  },
  methods: {
    schedule(failed) {
      if (this.guard !== null) { clearTimeout(this.guard); this.guard = null; }
      if (!this.active || !this.screenOn || (this.view !== "desktop" && this.view !== "keyboard") || this.job !== null) return;
      if (failed) {
        this.ready = false;
        state(this.view === "keyboard", false, !this.mouseOn, this.micOn);
      } else if (!this.ready) {
        state(this.view === "keyboard", false, !this.mouseOn, this.micOn);
        this.ready = true;
      }
      this.job = setTimeout(() => {
        this.job = null;
        this.requestFrame();
      }, failed ? 500 : 0);
    },
    requestFrame() {
      if (!this.active || !this.screenOn || (this.view !== "desktop" && this.view !== "keyboard")) return;
      this.frame++;
    },
    poke() {
      if (!this.active || (this.view !== "desktop" && this.view !== "keyboard")) return;
      if (this.guard !== null) clearTimeout(this.guard);
      this.guard = setTimeout(() => { this.guard = null; this.schedule(true); }, 1500);
    },
    cancelFrame() {
      if (this.job !== null) clearTimeout(this.job);
      if (this.guard !== null) clearTimeout(this.guard);
      this.job = null;
      this.guard = null;
    },
    stop() {
      const running = this.active;
      if (running) {
        this.active = false;
        this.ready = false;
        this.audioOn = this.micOn = this.cameraOn = false;
        this.cancelFrame();
        this.touchRelease();
        this.finishPad(false);
        this.releaseKeys();
      }
      native.execShell("/bin/sh -lc " + quote(RUN) + " -- stop >/dev/null 2>&1 &");
    },
    toggleAudio() {
      if (this.micOn) return;
      this.audioOn = !this.audioOn;
      control([0x27, this.audioOn ? 1 : 0]);
    },
    toggleMic() {
      if (this.cameraOn) return;
      this.micOn = !this.micOn;
      if (this.micOn && this.audioOn) {
        this.audioOn = false;
        control([0x27, 0]);
      }
      control([0x28, this.micOn ? 1 : 0], true);
      state(true, true, true, this.micOn);
    },
    toggleCamera() {
      this.cameraOn = !this.cameraOn;
      this.micOn = this.cameraOn;
      control([0x28, this.micOn ? 1 : 0], true);
      control([0x29, this.cameraOn ? 1 : 0], true);
      state(true, true, true, this.micOn);
    },
    wakeComputer() {
      native.execShell("/bin/sh -lc " + quote(RUN) + " -- wake 100.84.66.41 < /dev/null > /dev/null 2>&1 &");
    },
    toggleScreen() {
      this.screenOn = !this.screenOn;
    },
    toggleMouse() {
      this.touchRelease();
      this.finishPad(false);
      if (this.touchOn) this.touchOn = false;
      else if (this.mouseOn) {
        this.mouseOn = false;
        this.touchOn = true;
      }
      else this.mouseOn = true;
      state(true, true, !this.mouseOn, this.micOn);
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
      this.touchRelease();
      this.finishPad(false);
      this.releaseKeys();
      this.cancelFrame();
      this.view = "tools";
      state(true, true, true, this.micOn);
    },
    showKeyboard() {
      this.view = "keyboard";
      if (this.screenOn) this.frame++;
      state(true, !this.screenOn, !this.mouseOn, this.micOn);
    },
    showFiles() {
      this.view = "files";
      this.fileSession++;
      this.fileOffset.pen = this.fileOffset.windows = 0;
      this.fileState = { pen: [], windows: [], revision: 0 };
      state(true, true, true, this.micOn);
      this.fileAction("reset");
    },
    showDesktop() {
      this.touchRelease();
      this.finishPad(false);
      this.releaseKeys();
      this.view = "desktop";
      state(false, !this.screenOn, !this.mouseOn, this.micOn);
      this.requestFrame();
    },
    fileAction(action) {
      if (action.indexOf("open/pen/") === 0) this.fileOffset.pen = 0;
      if (action.indexOf("open/windows/") === 0) this.fileOffset.windows = 0;
      if (action.indexOf("transfer/") === 0) {
        const side = action === "transfer/push" ? "pen" : "windows";
        if (!this.fileState[side].some(entry => entry.state === 1)) return;
      }
      native.execShell("/bin/sh -lc " + quote(RUN) + " -- files " + action);
      this.loadFiles();
    },
    fileChanged(session) {
      if (this.view === "files" && session === this.fileSession) this.loadFiles();
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
      this.fileOffset[side] = Math.max(0, Math.min(Math.max(0, this.fileState[side].length - fileRows), offset));
    },
    fileTouchMove(side, event) {
      const touch = this.fileTouch;
      if (!touch || touch.side !== side) return;
      const distance = touch.y - this.fileY(event);
      if (Math.abs(distance) < 9) return;
      this.fileScrolled = true;
      this.fileScroll(side, touch.offset + Math.round(distance / 27));
    },
    fileTouchEnd(side, event) {
      const touch = this.fileTouch;
      this.fileTouch = null;
      if (!touch || touch.side !== side) return;
      const distance = touch.y - this.fileY(event);
      if (Math.abs(distance) >= 9) {
        this.fileScrolled = true;
        this.fileScroll(side, touch.offset + Math.round(distance / 27));
        setTimeout(() => { this.fileScrolled = false; }, 90);
      }
    },
    padTouches(event) {
      const active = event && event.touches;
      const changed = event && event.changedTouches;
      return active && active.length ? active : changed && changed.length ? changed : event ? [event] : [];
    },
    touchPoint(touch, event) {
      const point = this.padPosition(touch);
      const target = event && (event.currentTarget || event.target);
      const rect = target && target.getBoundingClientRect && target.getBoundingClientRect();
      return {
        id: point.id,
        x: Math.max(0, Math.round(point.x - (rect ? rect.left : 0))),
        y: Math.max(0, Math.round(point.y - (rect ? rect.top : 0)))
      };
    },
    touchFrame(event, ending) {
      if (this.view !== "desktop" || !this.touchOn) return;
      const active = event && event.touches || (ending ? [] : [event]);
      const changed = event && event.changedTouches || (event ? [event] : []);
      const points = [];
      for (let index = 0; index < active.length; index++) {
        const point = this.touchPoint(active[index], event);
        point.phase = this.directTouches[point.id] ? 2 : 1;
        points.push(point);
      }
      if (ending) {
        for (let index = 0; index < changed.length; index++) {
          const point = this.touchPoint(changed[index], event);
          if (!active.some(item => (item.identifier === undefined ? 0 : item.identifier) === point.id)) {
            const previous = this.directTouches[point.id] || point;
            points.push({ id: point.id, phase: 3, x: previous.x, y: previous.y });
          }
        }
      }
      if (!points.length) return;
      const packet = [0x2a, points.length];
      points.forEach(point => packet.push(point.id, point.phase, point.x >> 8, point.x, point.y >> 8, point.y));
      control(packet, true);
      points.forEach(point => {
        if (point.phase === 3) delete this.directTouches[point.id];
        else this.directTouches[point.id] = point;
      });
    },
    touchRelease() {
      const points = Object.keys(this.directTouches).map(id => {
        const point = this.directTouches[id];
        return [Number(id), 3, point.x, point.y];
      });
      this.directTouches = {};
      if (!points.length) return;
      const packet = [0x2a, points.length];
      points.forEach(point => packet.push(point[0], point[1], point[2] >> 8, point[2], point[3] >> 8, point[3]));
      control(packet, true);
    },
    padPosition(touch) {
      return {
        id: touch.identifier === undefined ? 0 : touch.identifier,
        x: touch.pageX === undefined ? (touch.clientX === undefined ? (touch.screenX === undefined ? touch.x || 0 : touch.screenX) : touch.clientX) : touch.pageX,
        y: touch.pageY === undefined ? (touch.clientY === undefined ? (touch.screenY === undefined ? touch.y || 0 : touch.screenY) : touch.clientY) : touch.pageY
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
      if (this.view !== "keyboard" || !this.mouseOn) return;
      const touches = this.padTouches(event);
      const changed = event && event.changedTouches || touches;
      if (!changed.length) return;
      if (this.pad) {
        const pad = this.pad;
        if (touches.length > 1 && pad.max < 2) {
          pad.started = Date.now();
          pad.moved = false;
          if (pad.dragging) {
            control(button(1, false), true);
            pad.dragging = false;
          }
        }
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
      const pad = { id: point.id, x: point.x, y: point.y, originX: point.x, originY: point.y, started: Date.now(), max: touches.length || 1, moved: false, dragging: false, dx: 0, dy: 0, wheel: 0, press: null, timer: null };
      this.pad = pad;
      pad.press = setTimeout(() => {
        if (this.pad === pad && !pad.moved && pad.max === 1) {
          pad.dragging = true;
          control(button(1, true), true);
        }
      }, 300);
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
      const packet = dx || dy ? move(dy * mouseSensitivityY, -dx * mouseSensitivityX) : [];
      if (scroll) packet.push(...wheel(scroll));
      return packet;
    },
    padMove(event) {
      const pad = this.pad;
      const touches = this.padTouches(event);
      if (!this.mouseOn || !pad || !touches.length) return;
      pad.max = Math.max(pad.max, touches.length);
      const point = touches.length > 1 ? this.padFocus(touches) : this.padPoint(touches, pad.id);
      if (!point) return;
      const dx = point.x - pad.x;
      const dy = point.y - pad.y;
      pad.x = point.x;
      pad.y = point.y;
      if (!dx && !dy) return;
      this.poke();
      const wasMoved = pad.moved;
      const travel = touchDistance(point.x - pad.originX, point.y - pad.originY);
      if (!pad.moved && (touches.length > 1 ? travel >= gestureThreshold : travel > 3)) {
        pad.moved = true;
        if (pad.press !== null) clearTimeout(pad.press);
        pad.press = null;
      }
      if (touches.length > 1) {
        if (pad.moved) {
          pad.wheel += (wasMoved ? dy : point.y - pad.originY) * 8;
          this.padQueue(pad);
        }
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
      else if (click && !pad.moved && Date.now() - pad.started < 300) packet.push(...button(pad.max > 1 ? 2 : 1, true), ...button(pad.max > 1 ? 2 : 1, false));
      if (packet.length) { this.poke(); control(packet, pad.dragging || (click && !pad.moved)); }
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
      this.poke();
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
        held.timer = setTimeout(repeat, 180);
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
    touchSurface: { width: "100%", height: "100%", position: "absolute", top: 0, left: 0, backgroundColor: "transparent" },
    toolToggle: { position: "absolute", bottom: 9, right: 9, width: 27, height: 27, borderRadius: 14, backgroundColor: "transparent" },
    tools: { width: "100%", height: "100%", position: "relative" },
    toolRail: { position: "absolute", top: 9, left: 45, right: 9, height: 27, flexDirection: "row" },
    toolItem: { position: "relative", height: 27, flex: 1, marginRight: 3, flexDirection: "row", alignItems: "center", justifyContent: "center", backgroundColor: "#1D1E21", borderRadius: 9 },
    toolItemSelected: { backgroundColor: "#3474F0" },
    toolPower: { marginRight: 0 },
    toolText: { position: "relative", width: 36, height: 27, color: "#e6e1e5", fontSize: 18, lineHeight: "27px", textAlign: "center" },
    toolBack: { position: "absolute", top: 9, left: 9, width: 27, height: 27, alignItems: "center", justifyContent: "center", backgroundColor: "#1D1E21", borderRadius: 9 },
    toolStop: { position: "absolute", bottom: 9, left: 9, width: 27, height: 27, alignItems: "center", justifyContent: "center", backgroundColor: "#1D1E21", borderRadius: 9 },
    toolBackIcon: { width: 27, height: 27 },
    toolItemIcon: { width: 27, height: 27, marginRight: 3 },
    keyboard: { position: "absolute", bottom: 0, left: 0, width: "100%", height: "100%" },
    keyArea: { position: "absolute", top: 0, left: "13.5%", width: "73%", height: "100%", flexDirection: "column" },
    touchPad: { position: "absolute", top: 0, width: "13.5%", height: "100%", backgroundColor: "transparent" },
    touchPadLeft: { left: 0 },
    touchPadRight: { right: 0 },
    keyRow: { flex: 1, position: "relative" },
    button: { position: "absolute", top: 0, height: "100%", margin: 0, alignItems: "center", justifyContent: "center" },
    label: { color: "#77767b", opacity: 0.6, fontSize: 18, textAlign: "center" },
    selectedLabel: { color: "#a8c7fa", opacity: 1 },
    files: { width: "100%", height: "100%", position: "relative" },
    filePanel: { position: "absolute", top: 9, bottom: 9, left: 45, right: 9, backgroundColor: "#1D1E21", borderRadius: 9, overflow: "hidden" },
    fileRail: { position: "absolute", top: 0, left: 0, right: 0, height: 27, flexDirection: "row" },
    fileTitle: { position: "relative", height: 27, flex: 1, alignItems: "center", justifyContent: "center" },
    fileTitleText: { color: "#e6e1e5", fontSize: 18, lineHeight: "27px", textAlign: "center" },
    filePanels: { position: "absolute", top: 27, bottom: 0, left: 0, right: 0, flexDirection: "row" },
    filePane: { position: "relative", flex: 1, overflow: "hidden" },
    fileEntries: { position: "absolute", top: 0, left: 0, width: "100%", bottom: 30, overflow: "hidden" },
    fileRow: { position: "absolute", left: 0, width: "100%", height: 27 },
    fileEntryMain: { position: "absolute", top: 0, left: 0, width: "100%", height: 27 },
    fileEntrySelected: { backgroundColor: "#3474F0" },
    fileEntryFailed: { backgroundColor: "#4b252a" },
    fileEntryIconButton: { position: "absolute", top: 0, left: 3, width: 27, height: 27, alignItems: "center", justifyContent: "center" },
    fileEntryIcon: { width: 27, height: 27 },
    fileColumns: { position: "absolute", top: 0, left: 36, right: 9, height: 27, flexDirection: "row" },
    fileName: { position: "relative", flex: 1, minWidth: 0, height: 27, overflow: "hidden", whiteSpace: "nowrap", textOverflow: "ellipsis", color: "#e6e1e5", fontSize: 18, lineHeight: "27px" },
    fileDate: { position: "relative", minWidth: 0, height: 27, overflow: "hidden", whiteSpace: "nowrap", textOverflow: "ellipsis", color: "#a8adb8", fontSize: 18, lineHeight: "27px", textAlign: "left" },
    fileSize: { position: "relative", minWidth: 0, height: 27, overflow: "hidden", whiteSpace: "nowrap", textOverflow: "ellipsis", color: "#a8adb8", fontSize: 18, lineHeight: "27px", textAlign: "right" },
    fileProgressTrack: { position: "absolute", bottom: 0, left: 0, width: "100%", height: 3, backgroundColor: "#2b2d31" },
    fileProgress: { position: "absolute", top: 0, left: 0, height: "100%", backgroundColor: "#3474F0" },
    fileProgressSuccess: { backgroundColor: "#55b879" },
    fileProgressFailed: { backgroundColor: "#d13438" },
    fileTransfer: { position: "absolute", bottom: 0, left: 0, width: "100%", height: 27, alignItems: "center", justifyContent: "center" },
    fileTransferReady: { backgroundColor: "#3474F0" },
    fileTransferText: { color: "#e6e1e5", fontSize: 18, lineHeight: "27px", textAlign: "center" },
    fileEvent: { position: "absolute", width: 3, height: 3, opacity: 0 }
  }
};

const render = function () {
  const create = this.$createElement;
  const text = (value, classes, style) => create("text", {
    staticClass: ["font"].concat(classes), style, attrs: { value }
  });
  const remoteFrame = classes => this.screenOn ? create("image", {
    staticClass: classes,
    attrs: { src: "http://127.0.0.1:999/frame?" + this.frame },
    on: {
      load: event => this.schedule(!!event && event.success === false),
      error: () => this.schedule(true),
      touchstart: event => this.touchFrame(event, false),
      touchmove: event => this.touchFrame(event, false),
      touchend: event => this.touchFrame(event, true),
      touchcancel: event => this.touchFrame(event, true)
    }
  }) : create("div", { staticClass: classes });
  const touchSurface = () => this.touchOn ? create("div", {
    staticClass: ["touchSurface"],
    on: {
      touchstart: event => this.touchFrame(event, false),
      touchmove: event => this.touchFrame(event, false),
      touchend: event => this.touchFrame(event, true),
      touchcancel: event => this.touchFrame(event, true)
    }
  }) : null;
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
  const toolStop = action => create("div", { staticClass: ["toolStop"], on: { click: action } }, [
    icon(icons.stop, ["toolBackIcon"])
  ]);
  const date = value => {
    if (!value) return "";
    const timestamp = new Date(value * 1000);
    return timestamp.getFullYear() + "/" + (timestamp.getMonth() + 1) + "/" + timestamp.getDate();
  };
  const size = value => {
    const units = ["B", "KB", "MB", "GB"];
    let unit = 0;
    while (value >= 1024 && unit + 1 < units.length) {
      value = Math.floor(value / 1024);
      unit++;
    }
    return value + units[unit];
  };
  const details = entry => [date(entry.modified), entry.state >= 2 ? entry.progress + "%" : entry.directory ? "" : size(entry.size)];
  const fileLayout = entries => entries.reduce((layout, entry) => {
    const detail = details(entry);
    return { date: Math.max(layout.date, detail[0].length * 9), size: Math.max(layout.size, detail[1].length * 9) };
  }, { date: 0, size: 0 });
  if (this.view === "desktop") {
    return create("div", { key: "desktop", staticClass: ["root"] }, [
      remoteFrame(["desktop"]),
      touchSurface(),
      create("div", { staticClass: ["toolToggle"], on: { click: () => this.showTools() } })
    ]);
  }
  if (this.view === "tools") {
    return create("div", { key: "tools", staticClass: ["tools"] }, [
      toolBack(() => this.showDesktop()),
      toolStop(() => this.stop()),
      create("div", { staticClass: ["toolRail"] }, [
      create("div", { staticClass: ["toolItem", "toolScreen"].concat(this.screenOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleScreen() } }, [
        icon(icons.screen, ["toolItemIcon"]),
        text("显示", ["toolText"])
      ]),
      create("div", { staticClass: ["toolItem", "toolMouse"].concat(this.mouseOn || this.touchOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleMouse() } }, [
        icon(this.touchOn ? icons.touch : icons.mouse, ["toolItemIcon"]),
        text(this.touchOn ? "\u89e6\u63a7" : "\u9f20\u6807", ["toolText"])
      ]),
      create("div", { staticClass: ["toolItem", "toolKeyboard"], on: { click: () => this.showKeyboard() } }, [
        icon(icons.keyboard, ["toolItemIcon"]),
        text("键盘", ["toolText"])
      ]),
      create("div", { staticClass: ["toolItem", "toolCamera"].concat(this.cameraOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleCamera() } }, [
        icon(icons.camera, ["toolItemIcon"]),
        text("录像", ["toolText"])
      ]),
      create("div", { staticClass: ["toolItem", "toolMic"].concat(this.micOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleMic() } }, [
        icon(icons.mic, ["toolItemIcon"]),
        text("录音", ["toolText"])
      ]),
      create("div", { staticClass: ["toolItem", "toolSound"].concat(this.audioOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleAudio() } }, [
        icon(icons.sound, ["toolItemIcon"]),
        text("声音", ["toolText"])
      ]),
      create("div", { staticClass: ["toolItem", "toolFiles"], on: { click: () => this.showFiles() } }, [
        icon(icons.folder, ["toolItemIcon"]),
        text("文件", ["toolText"])
      ]),
      create("div", { staticClass: ["toolItem", "toolPower"], on: { click: () => this.wakeComputer() } }, [
        icon(icons.power, ["toolItemIcon"]),
        text("\u7535\u6e90", ["toolText"])
      ]),
      ])
    ]);
  }
  if (this.view === "keyboard") {
    return create("div", { key: "keyboard", staticClass: ["root"] }, [
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
  const fileRow = (side, entry, index, layout) => {
    var state = entry.state === 1 ? "fileEntrySelected" : entry.state === 3 ? "fileEntryFailed" : "";
    var primary = "open/" + side + "/" + index;
    var iconAction = "select/" + side + "/" + index;
    if (!entry.parent && !entry.directory) primary = iconAction;
    if (entry.parent) iconAction = primary;
    var progress = entry.state >= 2 ? entry.progress : -1;
    var progressClass = entry.state === 2 ? "fileProgressSuccess" : entry.state === 3 ? "fileProgressFailed" : "";
    var detail = details(entry);
    var contents = progress < 0 ? [] : [create("div", { staticClass: ["fileProgressTrack"] }, [
      create("div", { staticClass: ["fileProgress"].concat(progressClass ? [progressClass] : []), style: { width: progress + "%" } })
    ])];
    contents.push(create("div", { staticClass: ["fileColumns"] }, [
      text(entry.name, ["fileName"]),
      text(detail[0], ["fileDate"], { width: layout.date }),
      text(detail[1], ["fileSize"], { width: layout.size })
    ]));
    return create("div", {
      staticClass: ["fileRow"], style: { top: (index - this.fileOffset[side]) * 27 }
    }, [
      create("div", { staticClass: ["fileEntryMain"].concat(state ? [state] : []), on: { click: () => !this.fileScrolled && this.fileAction(primary) } }, contents),
      create("div", { staticClass: ["fileEntryIconButton"], on: { click: () => !this.fileScrolled && this.fileAction(iconAction) } }, [
        icon(entry.parent || entry.directory ? icons.folder : icons.file, ["fileEntryIcon"])
      ])
    ]);
  };
  const transfer = (side, action, label) => {
    const ready = this.fileState[side].some(entry => entry.state === 1);
    return create("div", {
      staticClass: ["fileTransfer"].concat(ready ? ["fileTransferReady"] : []), on: { click: () => ready && this.fileAction(action) }
    }, [text(label, ["fileTransferText"])]);
  };
  const list = (side, entries, action, label, layout) => {
    const offset = this.fileOffset[side];
    return create("div", {
      staticClass: ["filePane"],
      on: {
        touchstart: event => this.fileTouchStart(side, event),
        touchmove: event => this.fileTouchMove(side, event),
        touchend: event => this.fileTouchEnd(side, event)
      }
    }, [
      create("div", { staticClass: ["fileEntries"] }, entries.slice(offset, offset + fileRows).map((entry, index) => fileRow(side, entry, offset + index, layout))),
      transfer(side, action, label)
    ]);
  };
  const fileSession = this.fileSession;
  const fileRevision = this.fileState.revision;
  const layout = fileLayout(this.fileState.pen.concat(this.fileState.windows));
  return create("div", { key: "files", staticClass: ["files"] }, [
    toolBack(() => this.showTools()),
    toolStop(() => this.stop()),
    create("image", { key: "file-event-" + fileSession + "-" + fileRevision, staticClass: ["fileEvent"], attrs: { src: "http://127.0.0.1:999/files/watch/" + fileRevision }, on: { load: () => this.fileChanged(fileSession), error: () => this.fileChanged(fileSession) } }),
    create("div", { staticClass: ["filePanel"] }, [
      create("div", { staticClass: ["fileRail"] }, [
        create("div", { staticClass: ["fileTitle"] }, [text("\u672c\u5730", ["fileTitleText"])]),
        create("div", { staticClass: ["fileTitle"] }, [text("\u8fdc\u7aef", ["fileTitleText"])])
      ]),
      create("div", { staticClass: ["filePanels"] }, [
        list("pen", this.fileState.pen, "transfer/push", "\u4f20\u8fdc\u7aef", layout),
        list("windows", this.fileState.windows, "transfer/pull", "\u4f20\u672c\u5730", layout)
      ])
    ])
  ]);
};

script.render = render;
script.staticRenderFns = [];
script._compiled = true;
script.style = style._;
script.themes = style;

export default script;
