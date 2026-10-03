import global from "global"
import fs from "fs"
import sendControl from "vid"
const native = new global.Global()
const APPID = "__APPID__"
const RUN = "latest=; for app in /userdisk/*/data/mini_app/pkg/" + APPID + "/*/bin/run /userdisk/*/*/data/mini_app/pkg/" + APPID + "/*/bin/run /userdata/*/data/mini_app/pkg/" + APPID + "/*/bin/run /userdata/*/*/data/mini_app/pkg/" + APPID + '/*/bin/run; do [ -f "$app" ] || continue; if [ -z "$latest" ] || [ "$app" -nt "$latest" ]; then latest=$app; fi; done; [ -n "$latest" ] && exec /bin/sh "$latest" "$@"; exit 1'
const quote = (value) => "'" + value.replace(/'/g, "'\\''") + "'"
const command = "/bin/setsid /bin/sh -lc " + quote(RUN)
const STATE_BLOCKED = 1
const STATE_VIDEO_PAUSED = 2
const STATE_MOUSE_PAUSED = 4
const STATE_RECORDING = 8
const STATE_TOUCH = 16
const STATE_TOUCHPAD = 32
const FONT = "Google Sans Flex"
const icons = { screen: "view.png", mouse: "mouse.png", touch: "touch.png", keyboard: "kb.png", mic: "mic.png", camera: "cam.png", folder: "folder.png", file: "file.png", back: "back.png", refresh: "rf.png", exit: "exit.png", sound: "sound.png", power: "power.png" }

function control(bytes, ordered = false) {
   if (bytes[0] === 0x23) return sendControl(new Uint8Array(bytes).buffer)
   const packet = bytes.map((value) => "\\" + ("00" + (value & 255).toString(8)).slice(-3)).join("")
   const command = "/bin/sh -lc " + quote(RUN) + " -- input " + quote(packet) + " >/dev/null 2>&1"
   native.execShell(ordered ? command : command + " &")
}

function launch() {
   native.execShell(command + " < /dev/null > /dev/null 2>&1 &")
}

function state(blocked, paused, mousePaused = false, recording = false, touch = false, pad = false) {
   control([2, (blocked ? STATE_BLOCKED : 0) | (paused ? STATE_VIDEO_PAUSED : 0) | (mousePaused ? STATE_MOUSE_PAUSED : 0) | (recording ? STATE_RECORDING : 0) | (touch ? STATE_TOUCH : 0) | (pad ? STATE_TOUCHPAD : 0)], true)
}

const keys = { Esc: 27, Tab: 9, Back: 8, Enter: 13, Del: 46, End: 35, " ": 32, Left: 37, Up: 38, Right: 39, Down: 40, "`": 192, "-": 189, "=": 187, "[": 219, "]": 221, "\\": 220, ";": 186, "'": 222, ",": 188, ".": 190, "/": 191 }
const shifted = { "`": "~", "1": "!", "2": "@", "3": "#", "4": "$", "5": "%", "6": "^", "7": "&", "8": "*", "9": "(", "0": ")", "-": "_", "=": "+", "[": "{", "]": "}", "\\": "|", ";": ":", "'": '"', ",": "<", ".": ">", "/": "?" }
const arrows = { Left: "\u2190", Up: "\u2191", Right: "\u2192", Down: "\u2193" }
const keyboardWidth = 15
const fileRows = 7
const key = (label, span = 1) => ({ label, span })
const close = (span = 1) => ({ close: true, span })
const gap = (span = 1) => ({ span })
const row = (...entries) => {
   let column = 0
   return entries.reduce((placed, entry) => {
      const item = typeof entry === "string" ? key(entry) : entry
      if (item.label || item.close) placed.push({ label: item.label, close: item.close, column, span: item.span })
      column += item.span
      return placed
   }, [])
}
const keyRows = [row("Esc", "F1", "F2", "F3", "F4", "F5", "F6", close(), "F7", "F8", "F9", "F10", "F11", "F12", "Del"), row("`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", key("Back", 2)), row(key("Tab", 1.5), "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", key("\\", 1.5)), row(key("Caps", 1.75), "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", key("Enter", 2.25)), row(key("Shift", 2.25), "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", gap(0.75), "Up", gap()), row(key("Ctrl", 1.25), key("Win", 1.25), key("Alt", 1.25), key(" ", 6.25), key("End", 2), "Left", "Down", "Right")]
const modifierCodes = { Shift: 16, Ctrl: 17, Win: 91, Alt: 18 }

const deviceAddress = /^100\.(?:\d{1,3}\.){2}\d{1,3}$/
const parseDevices = (value) =>
   String(value)
      .split(/\r?\n/)
      .filter(Boolean)
      .map((line) => ({ device: line.replace(/^\*/, ""), active: line[0] === "*" }))
      .filter((entry) => deviceAddress.test(entry.device))

const script = {
   data() {
      const data = { active: true, job: null, held: [], fileTouch: null, fileScrolled: false, fileTransferPolling: false, fileTransferStarted: false, fileTransferSide: "", fileTransferTimer: null, devices: [], audioOn: false, screenOn: false, mouseOn: false, touchOn: false, micOn: false, cameraOn: false, view: "devices", fileState: { pen: [], windows: [] }, fileOffset: { pen: 0, windows: 0 }, fileRead: 0, caps: false, modifiers: { Shift: false, Ctrl: false, Win: false, Alt: false } }
      fs.readFile("/userdisk/PenDesk/CtrlDev", "utf8")
         .then((value) => {
            data.devices = parseDevices(value)
         })
         .catch(() => {})
      try {
         state(true, true, true, false)
      } catch {}
      return data
   },
   activated() {
      const resumed = !this.active
      this.active = true
      if (!resumed) {
         if (this.view === "devices") this.refreshDevices()
         return this.startVideo()
      }
      if (this.view === "devices") {
         state(true, true, true, this.micOn)
         this.refreshDevices()
         return
      }
      launch()
      state(this.view !== "desktop", !this.screenOn || this.view === "tools", !this.mouseOn, this.micOn, this.touchOn, this.view === "keyboard")
      this.startVideo()
   },
   deactivated() {
      this.active = false
      this.stopVideo()
      this.releaseKeys()
      state(true, true, true, this.micOn)
   },
   beforeDestroy() {
      this.exitDevice(false)
   },
   methods: {
      startVideo() {
         if (!this.active || !this.screenOn || (this.view !== "desktop" && this.view !== "keyboard")) return
         if (this.job !== null) clearTimeout(this.job)
         this.job = setTimeout(() => {
            this.job = null
            if (!this.active || !this.screenOn) return
            const video = this.$refs.remote
            if (video) video.play(0)
         }, 100)
      },
      stopVideo() {
         if (this.job !== null) clearTimeout(this.job)
         this.job = null
         const video = this.$refs.remote
         if (video) video.stop()
      },
      videoError() {
         if (!this.active || !this.screenOn || (this.view !== "desktop" && this.view !== "keyboard")) return
         this.stopVideo()
         this.job = setTimeout(() => {
            this.job = null
            this.startVideo()
         }, 500)
      },
      exitDevice(refresh) {
         if (this.fileTransferTimer !== null) clearTimeout(this.fileTransferTimer)
         this.fileTransferTimer = null
         this.fileTransferPolling = false
         this.active = false
         this.audioOn = this.micOn = this.cameraOn = false
         this.stopVideo()
         this.releaseKeys()
         native.execShell("/bin/sh -lc " + quote(RUN) + " -- stop >/dev/null 2>&1 &")
         if (refresh) {
            this.active = true
            this.view = "devices"
            state(true, true, true, false)
            this.refreshDevices()
         }
      },
      toggleAudio() {
         if (this.micOn) return
         this.audioOn = !this.audioOn
         control([0x27, this.audioOn ? 1 : 0])
      },
      setMic(enabled) {
         this.micOn = enabled
         if (this.micOn && this.audioOn) {
            this.audioOn = false
            control([0x27, 0])
         }
         control([0x28, this.micOn ? 1 : 0], true)
      },
      toggleMic() {
         if (this.cameraOn) return
         this.setMic(!this.micOn)
         state(true, true, true, this.micOn)
      },
      toggleCamera() {
         this.cameraOn = !this.cameraOn
         this.setMic(this.cameraOn)
         control([0x29, this.cameraOn ? 1 : 0], true)
         state(true, true, true, this.micOn)
      },
      wakeComputer() {
         native.execShell("/bin/sh -lc " + quote(RUN) + " -- wake 100.84.66.41 < /dev/null > /dev/null 2>&1 &")
      },
      toggleScreen() {
         this.screenOn = !this.screenOn
         state(this.view !== "desktop", !this.screenOn || this.view === "tools", !this.mouseOn, this.micOn, this.touchOn, this.view === "keyboard")
         if (this.screenOn) this.startVideo()
         else this.stopVideo()
      },
      toggleMouse() {
         if (this.touchOn) this.touchOn = false
         else if (this.mouseOn) {
            this.mouseOn = false
            this.touchOn = true
         } else this.mouseOn = true
         state(true, true, !this.mouseOn, this.micOn, this.touchOn)
      },
      releaseKeys() {
         const packet = []
         this.held.forEach((held) => {
            if (held.timer !== null) clearTimeout(held.timer)
         })
         Object.keys(modifierCodes).forEach((label) => {
            if (this.modifiers[label]) packet.push(0x23, 0, modifierCodes[label], 0)
         })
         if (packet.length) control(packet)
         Object.keys(modifierCodes).forEach((label) => {
            this.modifiers[label] = false
         })
         this.caps = false
         this.held = []
      },
      showTools() {
         this.releaseKeys()
         this.stopVideo()
         this.view = "tools"
         state(true, true, true, this.micOn)
      },
      showKeyboard() {
         this.view = "keyboard"
         if (this.screenOn) this.startVideo()
         state(true, !this.screenOn, !this.mouseOn, this.micOn, this.touchOn, true)
      },
      refreshDevices() {
         return fs.readFile("/userdisk/PenDesk/CtrlDev", "utf8").then(
            (value) => {
               this.devices = parseDevices(value)
            },
            () => {
               this.devices = []
            }
         )
      },
      selectDevice(device) {
         if (!deviceAddress.test(device)) return
         this.active = true
         this.screenOn = this.mouseOn = this.touchOn = this.audioOn = this.micOn = this.cameraOn = false
         control([0x27, 0])
         control([0x28, 0], true)
         control([0x29, 0], true)
         state(true, true, true, false)
         native.execShell("/bin/sh -lc " + quote(RUN) + " -- switch-device " + device + " < /dev/null > /dev/null 2>&1 &")
         this.view = "desktop"
         state(false, !this.screenOn, !this.mouseOn, this.micOn, this.touchOn)
         this.startVideo()
      },
      showFiles() {
         this.view = "files"
         this.fileOffset.pen = this.fileOffset.windows = 0
         state(true, true, true, this.micOn)
         if (this.fileTransferPolling) this.loadFiles()
         else this.fileAction("reset")
      },
      showDesktop() {
         this.releaseKeys()
         this.view = "desktop"
         state(false, !this.screenOn, !this.mouseOn, this.micOn, this.touchOn)
         this.startVideo()
      },
      fileAction(action) {
         if (this.fileTransferPolling) return
         if (action.indexOf("open/pen/") === 0) this.fileOffset.pen = 0
         if (action.indexOf("open/windows/") === 0) this.fileOffset.windows = 0
         if (action.indexOf("transfer/") === 0) {
            const side = action === "transfer/push" ? "pen" : "windows"
            if (!this.fileState[side].some((entry) => entry.state === 1)) return
            this.fileTransferPolling = true
            this.fileTransferStarted = false
            this.fileTransferSide = side
         }
         native.execShell("/bin/sh -lc " + quote(RUN) + " -- files " + action)
         if (this.fileTransferPolling) this.pollFileTransfer()
         else this.loadFiles()
      },
      pollFileTransfer() {
         if (!this.fileTransferPolling) return
         this.loadFiles().then(() => {
            if (!this.fileTransferPolling) return
            const entries = this.fileState[this.fileTransferSide]
            const active = entries.some((entry) => entry.state === 4)
            const pending = entries.some((entry) => entry.state === 1)
            const finished = entries.some((entry) => entry.state === 2 || entry.state === 3)
            if (active) this.fileTransferStarted = true
            if ((this.fileTransferStarted && !active) || (!active && !pending && finished)) {
               this.fileTransferPolling = false
               this.fileTransferStarted = false
               this.fileTransferSide = ""
               this.fileTransferTimer = null
               return
            }
            this.fileTransferTimer = setTimeout(() => this.pollFileTransfer(), 250)
         })
      },
      loadFiles() {
         const read = ++this.fileRead
         return fs
            .readFile("/tmp/pendesk-files.json", "utf8")
            .then((text) => {
               if (read !== this.fileRead) return
               const fileState = JSON.parse(typeof text === "string" ? text : String(text))
               if (Array.isArray(fileState.pen) && Array.isArray(fileState.windows)) {
                  this.fileState = fileState
                  this.fileScroll("pen", this.fileOffset.pen)
                  this.fileScroll("windows", this.fileOffset.windows)
               }
            })
            .catch(() => {})
      },
      fileY(event) {
         const touch = event && ((event.changedTouches && event.changedTouches[0]) || (event.touches && event.touches[0]) || event)
         return touch && (touch.pageY || touch.clientY || touch.y || touch.screenY || 0)
      },
      fileTouchStart(side, event) {
         this.fileScrolled = false
         this.fileTouch = { side, y: this.fileY(event), offset: this.fileOffset[side] }
      },
      fileScroll(side, offset) {
         this.fileOffset[side] = Math.max(0, Math.min(Math.max(0, this.fileState[side].length - fileRows), offset))
      },
      fileTouchMove(side, event) {
         const touch = this.fileTouch
         if (!touch || touch.side !== side) return
         this.fileDrag(touch, side, event)
      },
      fileTouchEnd(side, event) {
         const touch = this.fileTouch
         this.fileTouch = null
         if (!touch || touch.side !== side) return
         if (this.fileDrag(touch, side, event)) {
            setTimeout(() => {
               this.fileScrolled = false
            }, 90)
         }
      },
      fileDrag(touch, side, event) {
         const distance = touch.y - this.fileY(event)
         if (Math.abs(distance) < 9) return false
         this.fileScrolled = true
         this.fileScroll(side, touch.offset + Math.round(distance / 27))
         return true
      },
      keyPoints(event) {
         const changed = event && event.changedTouches
         return changed && changed.length ? changed : event ? [event] : []
      },
      tap(label) {
         let code
         if (/^F\d+$/.test(label)) {
            code = 111 + Number(label.slice(1))
         } else {
            code = keys[label] || label.charCodeAt(0)
         }
         const shift = this.caps && /^[A-Z]$/.test(label) && !this.modifiers.Shift
         const packet = shift ? [0x23, 0, 16, 1] : []
         packet.push(0x23, 0, code, 1, 0x23, 0, code, 0)
         if (shift) packet.push(0x23, 0, 16, 0)
         control(packet)
      },
      keyStart(entry, event) {
         this.keyPoints(event).forEach((point) => {
            const id = point.identifier === undefined ? 0 : point.identifier
            if (this.held.some((held) => held.id === id)) return
            const held = { id, entry, timer: null }
            this.held.push(held)
            const label = entry.label
            if (!label) return
            if (modifierCodes[label]) {
               this.modifiers[label] = !this.modifiers[label]
               control([0x23, 0, modifierCodes[label], this.modifiers[label] ? 1 : 0])
               return
            }
            if (label === "Caps") {
               this.caps = !this.caps
               return
            }
            this.tap(label)
            const repeat = () => {
               if (this.held.indexOf(held) < 0) return
               this.tap(label)
               held.timer = setTimeout(repeat, 30)
            }
            held.timer = setTimeout(repeat, 180)
         })
      },
      keyEnd(event) {
         this.keyPoints(event).forEach((point) => {
            const id = point.identifier === undefined ? 0 : point.identifier
            const index = this.held.findIndex((held) => held.id === id)
            if (index < 0) return
            const held = this.held[index]
            this.held.splice(index, 1)
            if (held.timer !== null) clearTimeout(held.timer)
            if (held.entry.close) return this.showTools()
         })
      }
   }
}

const style = {
   _: {
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
      toolExit: { position: "absolute", bottom: 9, left: 9, width: 27, height: 27, alignItems: "center", justifyContent: "center", backgroundColor: "#1D1E21", borderRadius: 9 },
      toolBackIcon: { width: 27, height: 27 },
      toolItemIcon: { width: 27, height: 27, marginRight: 3 },
      devices: { width: "100%", height: "100%", position: "relative" },
      deviceEntry: { position: "absolute", left: 45, right: 9, height: 27, paddingLeft: 9, alignItems: "center" },
      deviceEntryActive: { backgroundColor: "#3474F0" },
      deviceEntryText: { color: "#e6e1e5", fontSize: 18, lineHeight: "27px" },
      keyboard: { position: "absolute", bottom: 0, left: 0, width: "100%", height: "100%" },
      keyArea: { position: "absolute", top: 0, left: 0, width: "73%", height: "100%", flexDirection: "column" },
      touchPad: { position: "absolute", top: 0, right: 0, width: "27%", height: "100%", backgroundColor: "transparent" },
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
      fileEntrySuccess: { backgroundColor: "#55b879" },
      fileEntryFailed: { backgroundColor: "#d13438" },
      fileEntryIconButton: { position: "absolute", top: 0, left: 3, width: 27, height: 27, alignItems: "center", justifyContent: "center" },
      fileEntryIcon: { width: 27, height: 27 },
      fileColumns: { position: "absolute", top: 0, left: 36, right: 9, height: 27, flexDirection: "row" },
      fileName: { position: "relative", width: "64%", minWidth: 0, flexShrink: 1, height: 27, overflow: "hidden", whiteSpace: "nowrap", textOverflow: "ellipsis", color: "#e6e1e5", fontSize: 18, lineHeight: "27px" },
      fileDate: { position: "relative", width: "18%", flexShrink: 0, height: 27, overflow: "hidden", whiteSpace: "nowrap", textOverflow: "ellipsis", color: "#a8adb8", fontSize: 18, lineHeight: "27px", textAlign: "left" },
      fileSize: { position: "relative", width: "18%", flexShrink: 0, height: 27, overflow: "hidden", whiteSpace: "nowrap", textOverflow: "ellipsis", color: "#a8adb8", fontSize: 18, lineHeight: "27px", textAlign: "right" },
      fileProgressTrack: { position: "absolute", bottom: 0, left: 0, width: "100%", height: 3, backgroundColor: "#2b2d31" },
      fileProgress: { position: "absolute", top: 0, left: 0, height: "100%", backgroundColor: "#3474F0" },
      fileProgressSuccess: { backgroundColor: "#55b879" },
      fileProgressFailed: { backgroundColor: "#d13438" },
      fileTransfer: { position: "absolute", bottom: 0, left: 0, width: "100%", height: 27, alignItems: "center", justifyContent: "center" },
      fileTransferReady: { backgroundColor: "#3474F0" },
      fileTransferText: { color: "#e6e1e5", fontSize: 18, lineHeight: "27px", textAlign: "center" }
   }
}

const render = function () {
   const create = this.$createElement
   const text = (value, classes, style) => create("text", { staticClass: ["font"].concat(classes), style, attrs: { value } })
   const remoteFrame = (classes) => (this.screenOn ? create("video", { staticClass: classes, ref: "remote", attrs: { src: "http://127.0.0.1:999/native", enable_audio: false, playbin3: true }, on: { error: () => this.videoError(), completed: () => this.videoError() } }) : create("div", { staticClass: classes }))
   const touchSurface = () => (this.touchOn ? create("div", { staticClass: ["touchSurface"] }) : null)
   const icon = (value, classes) => create("div", { staticClass: classes, style: { backgroundImage: "url(" + value + ")", backgroundSize: "contain", backgroundRepeat: "no-repeat", backgroundPosition: "center" } })
   const keyButton = (entry) => {
      var value = entry.label || ""
      var shown = this.modifiers.Shift && shifted[value] ? shifted[value] : arrows[value] || value
      var selected = modifierCodes[value] ? this.modifiers[value] : value === "Caps" ? this.caps : this.held.some((item) => item.entry === entry)
      return create("div", { staticClass: ["button"], style: { left: (entry.column / keyboardWidth) * 100 + "%", width: (entry.span / keyboardWidth) * 100 + "%" }, on: { touchstart: (event) => this.keyStart(entry, event), touchend: (event) => this.keyEnd(event), touchcancel: () => this.releaseKeys() } }, shown ? [text(shown, ["label"].concat(selected ? ["selectedLabel"] : []))] : [])
   }
   const toolBack = (action) => create("div", { staticClass: ["toolBack"], on: { click: action } }, [icon(icons.back, ["toolBackIcon"])])
   const toolRefresh = (action) => create("div", { staticClass: ["toolBack"], on: { click: action } }, [icon(icons.refresh, ["toolBackIcon"])])
   const toolExit = (action) => create("div", { staticClass: ["toolExit"], on: { click: action } }, [icon(icons.exit, ["toolBackIcon"])])
   const date = (value) => {
      if (!value) return ""
      const timestamp = new Date(value * 1000)
      return timestamp.getFullYear() + "/" + (timestamp.getMonth() + 1) + "/" + timestamp.getDate()
   }
   const size = (value) => {
      const units = ["B", "KB", "MB", "GB"]
      let unit = 0
      while (value >= 1024 && unit + 1 < units.length) {
         value = Math.floor(value / 1024)
         unit++
      }
      return value + units[unit]
   }
   const details = (entry) => [date(entry.modified), entry.state === 4 ? entry.progress + "%" : entry.state === 2 ? "100%" : entry.state === 3 ? "失败" : entry.directory ? "" : size(entry.size)]
   if (this.view === "desktop") {
      return create("div", { key: "desktop", staticClass: ["root"] }, [remoteFrame(["desktop"]), touchSurface(), create("div", { staticClass: ["toolToggle"], on: { click: () => this.showTools() } })])
   }
   if (this.view === "tools") {
      return create("div", { key: "tools", staticClass: ["tools"] }, [
         toolBack(() => this.showDesktop()),
         toolExit(() => this.exitDevice(true)),
         create("div", { staticClass: ["toolRail"] }, [
            create("div", { staticClass: ["toolItem", "toolScreen"].concat(this.screenOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleScreen() } }, [icon(icons.screen, ["toolItemIcon"]), text("显示", ["toolText"])]),
            create("div", { staticClass: ["toolItem", "toolMouse"].concat(this.mouseOn || this.touchOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleMouse() } }, [icon(this.touchOn ? icons.touch : icons.mouse, ["toolItemIcon"]), text(this.touchOn ? "\u89e6\u63a7" : "\u9f20\u6807", ["toolText"])]),
            create("div", { staticClass: ["toolItem", "toolKeyboard"], on: { click: () => this.showKeyboard() } }, [icon(icons.keyboard, ["toolItemIcon"]), text("键盘", ["toolText"])]),
            create("div", { staticClass: ["toolItem", "toolCamera"].concat(this.cameraOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleCamera() } }, [icon(icons.camera, ["toolItemIcon"]), text("录像", ["toolText"])]),
            create("div", { staticClass: ["toolItem", "toolMic"].concat(this.micOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleMic() } }, [icon(icons.mic, ["toolItemIcon"]), text("录音", ["toolText"])]),
            create("div", { staticClass: ["toolItem", "toolSound"].concat(this.audioOn ? ["toolItemSelected"] : []), on: { click: () => this.toggleAudio() } }, [icon(icons.sound, ["toolItemIcon"]), text("声音", ["toolText"])]),
            create("div", { staticClass: ["toolItem", "toolFiles"], on: { click: () => this.showFiles() } }, [icon(icons.folder, ["toolItemIcon"]), text("文件", ["toolText"])]),
            create("div", { staticClass: ["toolItem", "toolPower"], on: { click: () => this.wakeComputer() } }, [icon(icons.power, ["toolItemIcon"]), text("\u7535\u6e90", ["toolText"])])
         ])
      ])
   }
   if (this.view === "devices") {
      const entries = this.devices.map((entry, index) => create("div", { key: entry.device, staticClass: ["deviceEntry"].concat(entry.active ? ["deviceEntryActive"] : []), style: { top: 9 + index * 27 }, on: { click: () => this.selectDevice(entry.device) } }, [text(entry.device, ["deviceEntryText"])]))
      return create("div", { key: "devices", staticClass: ["devices"] }, [toolRefresh(() => this.refreshDevices())].concat(entries))
   }
   if (this.view === "keyboard") {
      return create("div", { key: "keyboard", staticClass: ["root"] }, [
         remoteFrame(["desktop"]),
         create("div", { staticClass: ["keyboard"] }, [
            create(
               "div",
               { staticClass: ["keyArea"] },
               keyRows.map((row) => create("div", { staticClass: ["keyRow"] }, row.map(keyButton)))
            ),
            create("div", { staticClass: ["touchPad"] })
         ])
      ])
   }
   const fileRow = (side, entry, index) => {
      var state = entry.state === 1 ? "fileEntrySelected" : entry.state === 2 ? "fileEntrySuccess" : entry.state === 3 ? "fileEntryFailed" : ""
      var primary = "open/" + side + "/" + index
      var iconAction = "select/" + side + "/" + index
      if (!entry.parent && !entry.directory) primary = iconAction
      if (entry.parent) iconAction = primary
      var progress = entry.state >= 2 ? entry.progress : -1
      var progressClass = entry.state === 2 ? "fileProgressSuccess" : entry.state === 3 ? "fileProgressFailed" : ""
      var detail = details(entry)
      var contents = progress < 0 ? [] : [create("div", { staticClass: ["fileProgressTrack"] }, [create("div", { staticClass: ["fileProgress"].concat(progressClass ? [progressClass] : []), style: { width: progress + "%" } })])]
      contents.push(create("div", { staticClass: ["fileColumns"] }, [text(entry.name, ["fileName"]), text(detail[0], ["fileDate"]), text(detail[1], ["fileSize"])]))
      return create("div", { staticClass: ["fileRow"], style: { top: (index - this.fileOffset[side]) * 27 } }, [create("div", { staticClass: ["fileEntryMain"].concat(state ? [state] : []), on: { click: () => !this.fileScrolled && this.fileAction(primary) } }, contents), create("div", { staticClass: ["fileEntryIconButton"], on: { click: () => !this.fileScrolled && this.fileAction(iconAction) } }, [icon(entry.parent || entry.directory ? icons.folder : icons.file, ["fileEntryIcon"])])])
   }
   const transfer = (side, action, label) => {
      const entries = this.fileState[side]
      const ready = entries.some((entry) => entry.state === 1)
      const running = this.fileTransferPolling && this.fileTransferSide === side
      const batch = running ? entries.filter((entry) => entry.state >= 2) : []
      const total = batch.reduce((sum, entry) => sum + entry.total, 0)
      const done = batch.reduce((sum, entry) => sum + entry.done, 0)
      const completed = batch.filter((entry) => entry.state === 2 || entry.state === 3).length
      const progress = total ? Math.min(100, Math.floor((done * 100) / total)) : batch.length ? Math.floor((completed * 100) / batch.length) : 0
      const contents = []
      if (running) contents.push(create("div", { staticClass: ["fileProgressTrack"] }, [create("div", { staticClass: ["fileProgress"], style: { width: progress + "%" } })]))
      contents.push(text(running ? label + " " + progress + "%" : label, ["fileTransferText"]))
      return create("div", { staticClass: ["fileTransfer"].concat(ready ? ["fileTransferReady"] : []), on: { click: () => ready && !this.fileTransferPolling && this.fileAction(action) } }, contents)
   }
   const list = (side, entries, action, label) => {
      const offset = this.fileOffset[side]
      return create("div", { staticClass: ["filePane"], on: { touchstart: (event) => this.fileTouchStart(side, event), touchmove: (event) => this.fileTouchMove(side, event), touchend: (event) => this.fileTouchEnd(side, event) } }, [
         create(
            "div",
            { staticClass: ["fileEntries"] },
            entries.slice(offset, offset + fileRows).map((entry, index) => fileRow(side, entry, offset + index))
         ),
         transfer(side, action, label)
      ])
   }
   return create("div", { key: "files", staticClass: ["files"] }, [toolBack(() => this.showTools()), toolExit(() => this.exitDevice(true)), create("div", { staticClass: ["filePanel"] }, [create("div", { staticClass: ["fileRail"] }, [create("div", { staticClass: ["fileTitle"] }, [text("\u672c\u5730", ["fileTitleText"])]), create("div", { staticClass: ["fileTitle"] }, [text("\u8fdc\u7aef", ["fileTitleText"])])]), create("div", { staticClass: ["filePanels"] }, [list("pen", this.fileState.pen, "transfer/push", "\u4f20\u8fdc\u7aef"), list("windows", this.fileState.windows, "transfer/pull", "\u4f20\u672c\u5730")])])])
}

script.render = render
script.staticRenderFns = []
script._compiled = true
script.style = style._
script.themes = style
export default script
