use crate::all::protocol::{Video, View};
use crate::gpu::Gpu;
use crate::signal::Signal;
use crate::viewport::{Bounds, Viewport};
use std::sync::Arc;
use windows::Foundation::TypedEventHandler;
use windows::Graphics::Capture::{
    Direct3D11CaptureFrame, Direct3D11CaptureFramePool, GraphicsCaptureItem, GraphicsCaptureSession,
};
use windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
use windows::Graphics::DirectX::DirectXPixelFormat;
use windows::Win32::Foundation::{POINT, RECT};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_RENDER_TARGET, D3D11_TEX2D_VPIV, D3D11_TEX2D_VPOV, D3D11_TEXTURE2D_DESC,
    D3D11_USAGE_DEFAULT, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, D3D11_VIDEO_PROCESSOR_COLOR_SPACE,
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC, D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC,
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC_0, D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC,
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0, D3D11_VIDEO_PROCESSOR_ROTATION_270,
    D3D11_VIDEO_PROCESSOR_STREAM, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
    D3D11_VPIV_DIMENSION_TEXTURE2D, D3D11_VPOV_DIMENSION_TEXTURE2D, ID3D11Texture2D,
    ID3D11VideoContext, ID3D11VideoDevice, ID3D11VideoProcessor, ID3D11VideoProcessorEnumerator,
    ID3D11VideoProcessorInputView, ID3D11VideoProcessorOutputView,
};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_NV12, DXGI_RATIONAL, DXGI_SAMPLE_DESC};
use windows::Win32::Graphics::Gdi::{MONITOR_DEFAULTTOPRIMARY, MonitorFromPoint};
use windows::Win32::System::WinRT::Direct3D11::{
    CreateDirect3D11DeviceFromDXGIDevice, IDirect3DDxgiInterfaceAccess,
};
use windows::Win32::System::WinRT::Graphics::Capture::IGraphicsCaptureItemInterop;
use windows::core::{Interface, factory};
pub struct Screen {
    bounds: Bounds,
    enumerator: ID3D11VideoProcessorEnumerator,
    output_view: ID3D11VideoProcessorOutputView,
    pool: Direct3D11CaptureFramePool,
    processor: ID3D11VideoProcessor,
    session: GraphicsCaptureSession,
    source: Option<(Direct3D11CaptureFrame, ID3D11VideoProcessorInputView)>,
    target: ID3D11Texture2D,
    token: i64,
    video_context: ID3D11VideoContext,
    video_device: ID3D11VideoDevice,
    pub view: View,
    pub dimensions: (u32, u32),
}
pub struct Frame<'a> {
    pub texture: &'a ID3D11Texture2D,
}
impl Screen {
    pub fn new(
        gpu: &Gpu,
        video: Video,
        viewport: &mut Viewport,
        signal: &Arc<Signal>,
    ) -> Result<Self, String> {
        let bounds = Bounds::current()?;
        viewport.update(bounds);
        Self::build(gpu, video, viewport.frame_size(), bounds, signal)
            .map_err(|error| error.to_string())
    }
    fn build(
        gpu: &Gpu,
        video: Video,
        canvas: Video,
        bounds: Bounds,
        signal: &Arc<Signal>,
    ) -> windows::core::Result<Self> {
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
            let target = Self::texture(gpu, u32::from(canvas.width), u32::from(canvas.height))?;
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
                OutputWidth: u32::from(canvas.width),
                OutputHeight: u32::from(canvas.height),
                Usage: D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
            };
            let enumerator = video_device.CreateVideoProcessorEnumerator(&content)?;
            let processor = video_device.CreateVideoProcessor(&enumerator, 0)?;
            video_context.VideoProcessorSetStreamRotation(
                &processor,
                0,
                true,
                D3D11_VIDEO_PROCESSOR_ROTATION_270,
            );
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
                &D3D11_VIDEO_PROCESSOR_COLOR_SPACE { _bitfield: 0 },
            );
            let notify = Arc::clone(signal);
            let token = pool.FrameArrived(&TypedEventHandler::new(move |_, _| {
                notify.set();
                Ok(())
            }))?;
            session.StartCapture()?;
            Ok(Self {
                bounds,
                enumerator,
                output_view: output_view.unwrap(),
                pool,
                processor,
                session,
                source: None,
                target,
                token,
                video_context,
                video_device,
                dimensions: (u32::from(canvas.width), u32::from(canvas.height)),
                view: View {
                    height: video.height,
                    width: video.width,
                    x: 0,
                    y: 0,
                },
            })
        }
    }
    fn texture(gpu: &Gpu, width: u32, height: u32) -> windows::core::Result<ID3D11Texture2D> {
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_NV12,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_RENDER_TARGET.0 as u32,
            CPUAccessFlags: 0,
            MiscFlags: 0,
        };
        let mut texture = None;
        unsafe {
            gpu.device
                .CreateTexture2D(&desc, None, Some(&mut texture))?
        };
        Ok(texture.unwrap())
    }
    pub fn changed(&self) -> bool {
        Bounds::current().is_ok_and(|bounds| bounds != self.bounds)
    }
    pub fn capture(
        &mut self,
        (x, y, width, height): (i32, i32, i32, i32),
    ) -> Result<Option<Frame<'_>>, String> {
        self.acquire().map_err(|error| error.to_string())?;
        let Some((_, input_view)) = &self.source else {
            return Ok(None);
        };
        let rect = RECT {
            left: x - self.bounds.left,
            top: y - self.bounds.top,
            right: x - self.bounds.left + width,
            bottom: y - self.bounds.top + height,
        };
        let mut stream = D3D11_VIDEO_PROCESSOR_STREAM {
            Enable: true.into(),
            OutputIndex: 0,
            InputFrameOrField: 0,
            PastFrames: 0,
            FutureFrames: 0,
            ppPastSurfaces: std::ptr::null_mut(),
            pInputSurface: std::mem::ManuallyDrop::new(Some(input_view.clone())),
            ppFutureSurfaces: std::ptr::null_mut(),
            ppPastSurfacesRight: std::ptr::null_mut(),
            pInputSurfaceRight: std::mem::ManuallyDrop::new(None),
            ppFutureSurfacesRight: std::ptr::null_mut(),
        };
        unsafe {
            self.video_context.VideoProcessorSetStreamSourceRect(
                &self.processor,
                0,
                true,
                Some(&rect),
            );
            let result = self.video_context.VideoProcessorBlt(
                &self.processor,
                &self.output_view,
                0,
                std::slice::from_ref(&stream),
            );
            std::mem::ManuallyDrop::drop(&mut stream.pInputSurface);
            result.map_err(|error| error.to_string())?;
        }
        Ok(Some(Frame {
            texture: &self.target,
        }))
    }
    fn acquire(&mut self) -> windows::core::Result<()> {
        let mut latest: Option<Direct3D11CaptureFrame> = None;
        while let Ok(frame) = self.pool.TryGetNextFrame() {
            if let Some(previous) = latest.replace(frame) {
                previous.Close()?;
            }
        }
        if let Some(frame) = latest {
            let surface = frame.Surface()?;
            let access: IDirect3DDxgiInterfaceAccess = surface.cast()?;
            let texture: ID3D11Texture2D = unsafe { access.GetInterface()? };
            let mut input_view = None;
            unsafe {
                self.video_device.CreateVideoProcessorInputView(
                    &texture,
                    &self.enumerator,
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
            }
            if let Some((previous, _)) = self.source.replace((frame, input_view.unwrap())) {
                previous.Close()?;
            }
        }
        Ok(())
    }
}
impl Drop for Screen {
    fn drop(&mut self) {
        let _ = self.session.Close();
        let _ = self.pool.RemoveFrameArrived(self.token);
        if let Some((frame, _)) = self.source.take() {
            let _ = frame.Close();
        }
        let _ = self.pool.Close();
    }
}
