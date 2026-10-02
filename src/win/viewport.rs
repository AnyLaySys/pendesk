use crate::all::protocol::Video;
use std::mem::size_of;
use std::time::Instant;
use windows::Win32::Foundation::POINT;
use windows::Win32::Graphics::Gdi::{
    GetMonitorInfoW, HMONITOR, MONITOR_DEFAULTTOPRIMARY, MONITORINFO, MonitorFromPoint,
};
use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;

#[derive(Clone, Copy, PartialEq)]
pub(crate) struct Bounds {
    pub(crate) left: i32,
    pub(crate) top: i32,
    pub(crate) width: u32,
    pub(crate) height: u32,
}

pub struct Viewport {
    bounds: Bounds,
    center_x: f64,
    center_y: f64,
    render_x: f64,
    render_y: f64,
    rendered_at: Instant,
    pointer_x: f64,
    pointer_y: f64,
    video: Video,
    zoom: f64,
    pub input_sequence: u32,
}

impl Viewport {
    pub fn new(video: Video) -> Result<Self, String> {
        let bounds = Bounds::current()?;
        let mut viewport = Self {
            bounds,
            center_x: f64::from(bounds.width) / 2.0,
            center_y: f64::from(bounds.height) / 2.0,
            render_x: f64::from(bounds.width) / 2.0,
            render_y: f64::from(bounds.height) / 2.0,
            rendered_at: Instant::now(),
            pointer_x: f64::from(bounds.width) / 2.0,
            pointer_y: f64::from(bounds.height) / 2.0,
            video,
            zoom: 1.0,
            input_sequence: 0,
        };
        viewport.sync_pointer();
        viewport.center_x = viewport.pointer_x;
        viewport.center_y = viewport.pointer_y;
        viewport.limit();
        viewport.render_x = viewport.center_x;
        viewport.render_y = viewport.center_y;
        Ok(viewport)
    }

    pub fn zoom(&mut self, delta: i16) {
        if delta == 0 {
            return;
        }
        self.sync_pointer();
        self.follow_pointer();
        let (left, top, width, height) = self.capture();
        let anchor_x = (self.pointer_x - f64::from(left - self.bounds.left)) / f64::from(width);
        let anchor_y = (self.pointer_y - f64::from(top - self.bounds.top)) / f64::from(height);
        self.zoom = (self.zoom * (f64::from(delta) / 160.0).exp()).clamp(1.0, 9.0);
        let (_, _, width, height) = self.capture();
        self.center_x = self.pointer_x + (0.5 - anchor_x) * f64::from(width);
        self.center_y = self.pointer_y + (0.5 - anchor_y) * f64::from(height);
        self.limit();
    }

    pub fn move_pointer(&mut self, delta_x: i16, delta_y: i16) -> (u16, u16) {
        if delta_x == 0 && delta_y == 0 {
            self.sync_pointer();
        }
        let (_, _, view_width, view_height) = self.capture();
        let canvas = self.canvas();
        let (view_width, view_height) = if canvas != self.video {
            (f64::from(self.bounds.width - 1) * f64::from(self.video.height) / f64::from(canvas.height - 1),
             f64::from(self.bounds.height - 1) * f64::from(self.video.width) / f64::from(canvas.width - 1))
        } else { (f64::from(view_width), f64::from(view_height)) };
        let width = self.bounds.width.saturating_sub(1);
        let height = self.bounds.height.saturating_sub(1);
        self.pointer_x = (self.pointer_x
            - f64::from(delta_y) * view_width / f64::from(u16::MAX))
        .clamp(0.0, f64::from(width));
        self.pointer_y = (self.pointer_y
            + f64::from(delta_x) * view_height / f64::from(u16::MAX))
        .clamp(0.0, f64::from(height));
        self.follow_pointer();
        self.pointer()
    }

    pub fn canvas(&self) -> Video {
        let height = (u64::from(self.bounds.height) * u64::from(self.video.height)
            / u64::from(self.bounds.width)).div_ceil(2) * 2;
        if self.zoom > 1.0001 || height <= u64::from(self.video.width) || height > 1080 || self.video.height > 1920 {
            return self.video;
        }
        Video { width: height as u16, height: self.video.height }
    }

    pub fn capture_source(&self) -> (i32, i32, i32, i32) {
        if self.canvas() != self.video {
            (self.bounds.left, self.bounds.top, self.bounds.width as i32, self.bounds.height as i32)
        } else { self.capture() }
    }

    pub fn origin(&self) -> (u16, u16) {
        let canvas = self.canvas();
        if canvas == self.video { return (0, 0); }
        let (x, y, _, _) = self.capture();
        (((x - self.bounds.left) as u64 * u64::from(canvas.height) / u64::from(self.bounds.width)) as u16,
         ((y - self.bounds.top) as u64 * u64::from(canvas.width) / u64::from(self.bounds.height)) as u16)
    }

    pub fn pointer(&self) -> (u16, u16) {
        ((self.pointer_x * f64::from(u16::MAX) / f64::from(self.bounds.width.saturating_sub(1).max(1))).round() as u16,
         (self.pointer_y * f64::from(u16::MAX) / f64::from(self.bounds.height.saturating_sub(1).max(1))).round() as u16)
    }

    pub fn advance(&mut self, now: Instant) {
        let elapsed = now.duration_since(self.rendered_at).as_secs_f64();
        self.rendered_at = now;
        let amount = 1.0 - (-elapsed / 0.008).exp();
        self.render_x += (self.center_x - self.render_x) * amount;
        self.render_y += (self.center_y - self.render_y) * amount;
    }

    fn capture(&self) -> (i32, i32, i32, i32) {
        self.area(self.render_x, self.render_y)
    }

    fn area(&self, center_x: f64, center_y: f64) -> (i32, i32, i32, i32) {
        let width = f64::from(self.bounds.width);
        let height = f64::from(self.bounds.height);
        let scale = (f64::from(self.video.height) / width)
            .max(f64::from(self.video.width) / height)
            * self.zoom;
        let view_width = (f64::from(self.video.height) / scale).min(width);
        let view_height = (f64::from(self.video.width) / scale).min(height);
        let width = view_width.round() as i32;
        let height = view_height.round() as i32;
        let x = (center_x - view_width / 2.0).round() as i32;
        let y = (center_y - view_height / 2.0).round() as i32;
        (
            self.bounds.left + x.clamp(0, self.bounds.width as i32 - width),
            self.bounds.top + y.clamp(0, self.bounds.height as i32 - height),
            width,
            height,
        )
    }

    pub fn touch_point(&self, x: u16, y: u16) -> (i32, i32) {
        let canvas = self.canvas();
        if canvas != self.video {
            return (self.bounds.left + (u32::from(x).min(u32::from(canvas.height - 1)) * self.bounds.width.saturating_sub(1) / u32::from(canvas.height - 1)) as i32,
                    self.bounds.top + (u32::from(y).min(u32::from(canvas.width - 1)) * self.bounds.height.saturating_sub(1) / u32::from(canvas.width - 1)) as i32);
        }
        let (left, top, width, height) = self.capture();
        let max_x = u32::from(self.video.height.saturating_sub(1));
        let max_y = u32::from(self.video.width.saturating_sub(1));
        (
            left + (u32::from(x).min(max_x) * width.saturating_sub(1) as u32 / max_x.max(1)) as i32,
            top + (u32::from(y).min(max_y) * height.saturating_sub(1) as u32 / max_y.max(1)) as i32,
        )
    }

    fn sync_pointer(&mut self) {
        let mut point = POINT::default();
        if unsafe { GetCursorPos(&mut point) }.is_ok() {
            let actual_x = f64::from(point.x - self.bounds.left)
                .clamp(0.0, f64::from(self.bounds.width.saturating_sub(1)));
            let actual_y = f64::from(point.y - self.bounds.top)
                .clamp(0.0, f64::from(self.bounds.height.saturating_sub(1)));
            if (actual_x - self.pointer_x).abs() > 1.0 || (actual_y - self.pointer_y).abs() > 1.0 {
                self.pointer_x = actual_x;
                self.pointer_y = actual_y;
            }
        }
    }

    fn follow_pointer(&mut self) {
        let (left, top, width, height) = self.area(self.center_x, self.center_y);
        let offset_x = self.pointer_x - f64::from(left - self.bounds.left);
        let offset_y = self.pointer_y - f64::from(top - self.bounds.top);
        let width = f64::from(width - 1);
        let height = f64::from(height - 1);
        let margin_x = width * (24.0 / f64::from(self.video.height)).min(0.25);
        let margin_y = height * (24.0 / f64::from(self.video.width)).min(0.25);
        self.center_x += offset_x - offset_x.clamp(margin_x, width - margin_x);
        self.center_y += offset_y - offset_y.clamp(margin_y, height - margin_y);
        self.limit();
    }

    fn limit(&mut self) {
        let (_, _, width, height) = self.capture();
        self.center_x = self.center_x.clamp(
            f64::from(width) / 2.0,
            f64::from(self.bounds.width) - f64::from(width) / 2.0,
        );
        self.center_y = self.center_y.clamp(
            f64::from(height) / 2.0,
            f64::from(self.bounds.height) - f64::from(height) / 2.0,
        );
    }

    pub(crate) fn update(&mut self, bounds: Bounds) {
        let x = self.center_x / f64::from(self.bounds.width);
        let y = self.center_y / f64::from(self.bounds.height);
        self.bounds = bounds;
        self.center_x = x * f64::from(bounds.width);
        self.center_y = y * f64::from(bounds.height);
        self.sync_pointer();
        self.limit();
    }
}

impl Bounds {
    pub(crate) fn current() -> Result<Self, String> {
        let monitor = unsafe { MonitorFromPoint(POINT { x: 0, y: 0 }, MONITOR_DEFAULTTOPRIMARY) };
        Self::from_monitor(monitor)
    }

    fn from_monitor(monitor: HMONITOR) -> Result<Self, String> {
        let mut info = MONITORINFO {
            cbSize: size_of::<MONITORINFO>() as u32,
            ..Default::default()
        };
        if !unsafe { GetMonitorInfoW(monitor, &mut info) }.as_bool() {
            return Err("no desktop is available".into());
        }
        let width = info.rcMonitor.right - info.rcMonitor.left;
        let height = info.rcMonitor.bottom - info.rcMonitor.top;
        if width <= 0 || height <= 0 {
            return Err("no desktop is available".into());
        }
        Ok(Self {
            left: info.rcMonitor.left,
            top: info.rcMonitor.top,
            width: width as u32,
            height: height as u32,
        })
    }
}
