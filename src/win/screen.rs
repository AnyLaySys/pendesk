use crate::all::protocol::{Video, View};
use crate::gpu::Gpu;
use std::mem::size_of;
use std::thread;
use std::time::{Duration, Instant};
use windows::Graphics::Capture::{
    Direct3D11CaptureFramePool, GraphicsCaptureItem, GraphicsCaptureSession,
};
use windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
use windows::Graphics::DirectX::DirectXPixelFormat;
use windows::Win32::Foundation::{POINT, RECT};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_RENDER_TARGET, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_READ,
    D3D11_MAP_READ, D3D11_MAPPED_SUBRESOURCE, D3D11_TEX2D_VPIV, D3D11_TEX2D_VPOV,
    D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT, D3D11_USAGE_STAGING,
    D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE, D3D11_VIDEO_PROCESSOR_CONTENT_DESC,
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC, D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC_0,
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC, D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0,
    D3D11_VIDEO_PROCESSOR_STREAM, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL, D3D11_VPIV_DIMENSION_TEXTURE2D,
    D3D11_VPOV_DIMENSION_TEXTURE2D, ID3D11DeviceContext, ID3D11Texture2D, ID3D11VideoContext,
    ID3D11VideoDevice, ID3D11VideoProcessor, ID3D11VideoProcessorInputView,
    ID3D11VideoProcessorOutputView,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_RATIONAL, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Gdi::{
    GetMonitorInfoW, HMONITOR, MONITOR_DEFAULTTOPRIMARY, MONITORINFO, MonitorFromPoint,
};
use windows::Win32::System::WinRT::Direct3D11::{
    CreateDirect3D11DeviceFromDXGIDevice, IDirect3DDxgiInterfaceAccess,
};
use windows::Win32::System::WinRT::Graphics::Capture::IGraphicsCaptureItemInterop;
use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;
use windows::core::{Interface, factory};
#[derive(Clone, Copy, PartialEq)]
struct Bounds {
    left: i32,
    top: i32,
    width: u32,
    height: u32,
}
pub struct Viewport {
    bounds: Bounds,
    center_x: f64,
    center_y: f64,
    pointer_x: f64,
    pointer_y: f64,
    video: Video,
    zoom: f64,
}
pub struct Screen {
    bounds: Bounds,
    context: ID3D11DeviceContext,
    input_view: ID3D11VideoProcessorInputView,
    output_view: ID3D11VideoProcessorOutputView,
    pool: Direct3D11CaptureFramePool,
    processor: ID3D11VideoProcessor,
    session: GraphicsCaptureSession,
    source: ID3D11Texture2D,
    target: ID3D11Texture2D,
    staging: ID3D11Texture2D,
    pixels: Vec<u8>,
    width: u32,
    height: u32,
    have_frame: bool,
    video_context: ID3D11VideoContext,
    pub captured: u64,
    pub view: View,
}
pub struct Frame<'a> {
    pub pixels: &'a [u8],
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub captured: u64,
}
impl Viewport {
    pub fn new(video: Video) -> Result<Self, String> {
        let bounds = Bounds::current()?;
        let scale = (f64::from(video.height) / f64::from(bounds.width))
            .max(f64::from(video.width) / f64::from(bounds.height));
        let mut viewport = Self {
            bounds,
            center_x: f64::from(bounds.width) / 2.0,
            center_y: f64::from(bounds.height) / 2.0,
            pointer_x: f64::from(bounds.width) / 2.0,
            pointer_y: f64::from(bounds.height) / 2.0,
            video,
            zoom: scale.recip().clamp(1.0, 8.0),
        };
        viewport.sync_pointer();
        viewport.center_x = viewport.pointer_x;
        viewport.center_y = viewport.pointer_y;
        viewport.limit();
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
        self.zoom = (self.zoom * (f64::from(delta) / 160.0).exp()).clamp(1.0, 8.0);
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
        let width = self.bounds.width.saturating_sub(1);
        let height = self.bounds.height.saturating_sub(1);
        self.pointer_x = (self.pointer_x
            - f64::from(delta_y) * f64::from(view_width) / f64::from(u16::MAX))
        .clamp(0.0, f64::from(width));
        self.pointer_y = (self.pointer_y
            + f64::from(delta_x) * f64::from(view_height) / f64::from(u16::MAX))
        .clamp(0.0, f64::from(height));
        self.follow_pointer();
        (
            if width == 0 {
                0
            } else {
                (self.pointer_x * f64::from(u16::MAX) / f64::from(width)).round() as u16
            },
            if height == 0 {
                0
            } else {
                (self.pointer_y * f64::from(u16::MAX) / f64::from(height)).round() as u16
            },
        )
    }
    pub fn capture(&self) -> (i32, i32, i32, i32) {
        let width = f64::from(self.bounds.width);
        let height = f64::from(self.bounds.height);
        let scale = (f64::from(self.video.height) / width)
            .max(f64::from(self.video.width) / height)
            * self.zoom;
        let view_width = (f64::from(self.video.height) / scale).min(width);
        let view_height = (f64::from(self.video.width) / scale).min(height);
        let width = view_width.round() as i32;
        let height = view_height.round() as i32;
        let x = (self.center_x - view_width / 2.0).round() as i32;
        let y = (self.center_y - view_height / 2.0).round() as i32;
        (
            self.bounds.left + x.clamp(0, self.bounds.width as i32 - width),
            self.bounds.top + y.clamp(0, self.bounds.height as i32 - height),
            width,
            height,
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
        let (left, top, width, height) = self.capture();
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
    fn update(&mut self, bounds: Bounds) {
        let x = self.center_x / f64::from(self.bounds.width);
        let y = self.center_y / f64::from(self.bounds.height);
        self.bounds = bounds;
        self.center_x = x * f64::from(bounds.width);
        self.center_y = y * f64::from(bounds.height);
        self.sync_pointer();
        self.limit();
    }
}
impl Screen {
    pub fn new(gpu: &Gpu, video: Video, viewport: &mut Viewport) -> Result<Self, String> {
        let bounds = Bounds::current()?;
        viewport.update(bounds);
        Self::build(gpu, video, bounds).map_err(|error| error.to_string())
    }
    fn build(gpu: &Gpu, video: Video, bounds: Bounds) -> windows::core::Result<Self> {
        unsafe {
            let monitor = MonitorFromPoint(
                POINT {
                    x: bounds.left,
                    y: bounds.top,
                },
                MONITOR_DEFAULTTOPRIMARY,
            );
            let interop = factory::<GraphicsCaptureItem, IGraphicsCaptureItemInterop>()?;
            let item: GraphicsCaptureItem = interop.CreateForMonitor(monitor)?;
            let size = item.Size()?;
            let inspectable = CreateDirect3D11DeviceFromDXGIDevice(&gpu.dxgi()?)?;
            let device: IDirect3DDevice = inspectable.cast()?;
            let pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                &device,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                size,
            )?;
            let session = pool.CreateCaptureSession(&item)?;
            let _ = session.SetIsCursorCaptureEnabled(true);
            let _ = session.SetIsBorderRequired(false);
            let source = Self::texture(
                gpu,
                size.Width as u32,
                size.Height as u32,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
            )?;
            let target = Self::texture(
                gpu,
                u32::from(video.height),
                u32::from(video.width),
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D3D11_BIND_RENDER_TARGET.0 as u32,
            )?;
            let staging = Self::staging(gpu, u32::from(video.height), u32::from(video.width))?;
            let video_device: ID3D11VideoDevice = gpu.device.cast()?;
            let video_context: ID3D11VideoContext = gpu.context.cast()?;
            let content = D3D11_VIDEO_PROCESSOR_CONTENT_DESC {
                InputFrameFormat: D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
                InputFrameRate: DXGI_RATIONAL {
                    Numerator: 60,
                    Denominator: 1,
                },
                InputWidth: size.Width as u32,
                InputHeight: size.Height as u32,
                OutputFrameRate: DXGI_RATIONAL {
                    Numerator: 60,
                    Denominator: 1,
                },
                OutputWidth: u32::from(video.height),
                OutputHeight: u32::from(video.width),
                Usage: D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
            };
            let enumerator = video_device.CreateVideoProcessorEnumerator(&content)?;
            let processor = video_device.CreateVideoProcessor(&enumerator, 0)?;
            let mut input_view = None;
            video_device.CreateVideoProcessorInputView(
                &source,
                &enumerator,
                &D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC {
                    FourCC: 0,
                    ViewDimension: D3D11_VPIV_DIMENSION_TEXTURE2D,
                    Anonymous: D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC_0 {
                        Texture2D: D3D11_TEX2D_VPIV {
                            MipSlice: 0,
                            ArraySlice: 0,
                        },
                    },
                },
                Some(&mut input_view),
            )?;
            let mut output_view = None;
            video_device.CreateVideoProcessorOutputView(
                &target,
                &enumerator,
                &D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC {
                    ViewDimension: D3D11_VPOV_DIMENSION_TEXTURE2D,
                    Anonymous: D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0 {
                        Texture2D: D3D11_TEX2D_VPOV { MipSlice: 0 },
                    },
                },
                Some(&mut output_view),
            )?;
            video_context.VideoProcessorSetStreamColorSpace(
                &processor,
                0,
                &D3D11_VIDEO_PROCESSOR_COLOR_SPACE { _bitfield: 0 },
            );
            video_context.VideoProcessorSetOutputColorSpace(
                &processor,
                &D3D11_VIDEO_PROCESSOR_COLOR_SPACE { _bitfield: 16 },
            );
            session.StartCapture()?;
            Ok(Self {
                bounds,
                context: gpu.context.clone(),
                input_view: input_view.unwrap(),
                output_view: output_view.unwrap(),
                pool,
                processor,
                session,
                source,
                target,
                staging,
                pixels: vec![0; u32::from(video.height) as usize * u32::from(video.width) as usize * 3],
                width: u32::from(video.height),
                height: u32::from(video.width),
                have_frame: false,
                video_context,
                captured: 0,
                view: View {
                    height: video.height,
                    width: video.width,
                    x: 0,
                    y: 0,
                },
            })
        }
    }
    fn texture(
        gpu: &Gpu,
        width: u32,
        height: u32,
        format: windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT,
        bind: u32,
    ) -> windows::core::Result<ID3D11Texture2D> {
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: bind,
            CPUAccessFlags: 0,
            MiscFlags: 0,
        };
        let mut texture = None;
        unsafe { gpu.device.CreateTexture2D(&desc, None, Some(&mut texture))? };
        Ok(texture.unwrap())
    }
    fn staging(gpu: &Gpu, width: u32, height: u32) -> windows::core::Result<ID3D11Texture2D> {
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_B8G8R8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_STAGING,
            BindFlags: 0,
            CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
            MiscFlags: 0,
        };
        let mut texture = None;
        unsafe { gpu.device.CreateTexture2D(&desc, None, Some(&mut texture))? };
        Ok(texture.unwrap())
    }
    pub fn changed(&self) -> bool {
        Bounds::current().is_ok_and(|bounds| bounds != self.bounds)
    }
    pub fn capture(&mut self, (x, y, width, height): (i32, i32, i32, i32)) -> Result<Frame<'_>, String> {
        self.acquire().map_err(|error| error.to_string())?;
        if !self.have_frame {
            return Err("no desktop frame was captured".into());
        }
        let rect = RECT {
            left: x - self.bounds.left,
            top: y - self.bounds.top,
            right: x - self.bounds.left + width,
            bottom: y - self.bounds.top + height,
        };
        let stream = D3D11_VIDEO_PROCESSOR_STREAM {
            Enable: true.into(),
            OutputIndex: 0,
            InputFrameOrField: 0,
            PastFrames: 0,
            FutureFrames: 0,
            ppPastSurfaces: std::ptr::null_mut(),
            pInputSurface: std::mem::ManuallyDrop::new(Some(self.input_view.clone())),
            ppFutureSurfaces: std::ptr::null_mut(),
            ppPastSurfacesRight: std::ptr::null_mut(),
            pInputSurfaceRight: std::mem::ManuallyDrop::new(None),
            ppFutureSurfacesRight: std::ptr::null_mut(),
        };
        unsafe {
            self.video_context.VideoProcessorSetStreamSourceRect(&self.processor, 0, true, Some(&rect));
            self.video_context
                .VideoProcessorBlt(&self.processor, &self.output_view, 0, &[stream])
                .map_err(|error| error.to_string())?;
            self.context.CopyResource(&self.staging, &self.target);
            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            self.context
                .Map(&self.staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                .map_err(|error| error.to_string())?;
            let width = self.width as usize;
            let row = width * 3;
            for line in 0..self.height as usize {
                let source = (mapped.pData as *const u8).add(line * mapped.RowPitch as usize);
                let destination = self.pixels.as_mut_ptr().add(line * row);
                for column in 0..width {
                    let pixel = source.add(column * 4);
                    let out = destination.add(column * 3);
                    *out = *pixel;
                    *out.add(1) = *pixel.add(1);
                    *out.add(2) = *pixel.add(2);
                }
            }
            self.context.Unmap(&self.staging, 0);
        }
        Ok(Frame {
            pixels: &self.pixels,
            width: self.width,
            height: self.height,
            stride: self.width * 3,
            captured: self.captured,
        })
    }
    fn acquire(&mut self) -> windows::core::Result<()> {
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            let mut latest = None;
            while let Ok(frame) = self.pool.TryGetNextFrame() {
                latest = Some(frame);
            }
            if let Some(frame) = latest {
                let surface = frame.Surface()?;
                let access: IDirect3DDxgiInterfaceAccess = surface.cast()?;
                let texture: ID3D11Texture2D = unsafe { access.GetInterface()? };
                unsafe { self.context.CopyResource(&self.source, &texture) };
                frame.Close()?;
                self.have_frame = true;
                self.captured += 1;
                return Ok(());
            }
            if self.have_frame || Instant::now() >= deadline {
                return Ok(());
            }
            thread::sleep(Duration::from_millis(4));
        }
    }
}
impl Bounds {
    fn current() -> Result<Self, String> {
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
impl Drop for Screen {
    fn drop(&mut self) {
        let _ = self.session.Close();
        let _ = self.pool.Close();
    }
}
