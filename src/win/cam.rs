use std::sync::atomic::{AtomicBool, Ordering};

use std::sync::Arc;
use std::thread::{self, JoinHandle};

use crate::all::cam::Frame;
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender, TrySendError};
use std::time::{Duration, Instant};
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BI_RGB, BITMAPINFO, BITMAPINFOHEADER, BLACKNESS, BeginPaint, DIB_RGB_COLORS, EndPaint,
    PAINTSTRUCT, PatBlt, SRCCOPY, StretchDIBits,
};
use windows::Win32::Graphics::Imaging::{
    CLSID_WICImagingFactory, GUID_WICPixelFormat32bppBGRA, IWICImagingFactory,
    WICBitmapDitherTypeNone, WICBitmapPaletteTypeCustom, WICDecodeMetadataCacheOnLoad,
};
use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, CoCreateInstance, CoInitializeEx,
    CoUninitialize,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CREATESTRUCTW, CS_HREDRAW, CS_VREDRAW, CreateWindowExW, DefWindowProcW, DispatchMessageW,
    GWLP_USERDATA, GetClientRect, GetWindowLongPtrW, IDC_ARROW, LoadCursorW, MSG, PM_REMOVE,
    PostQuitMessage, RegisterClassW, SW_SHOW, SetWindowLongPtrW, SetWindowTextW, ShowWindow,
    TranslateMessage, WM_DESTROY, WM_ERASEBKGND, WM_NCCREATE, WM_PAINT, WNDCLASSW,
    WS_OVERLAPPEDWINDOW, WS_VISIBLE,
};
use windows::core::{PCWSTR, w};

struct Image {
    width: u32,
    height: u32,
    pixels: Vec<u8>,
}

struct WindowData {
    image: Option<Image>,
}

pub struct Window {
    sender: SyncSender<Frame>,
    closing: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Window {
    pub fn new() -> Result<Self, String> {
        let (sender, receiver) = mpsc::sync_channel(1);
        let (ready_sender, ready_receiver) = mpsc::channel();
        let closing = Arc::new(AtomicBool::new(false));
        let thread_closing = Arc::clone(&closing);
        let thread = thread::Builder::new()
            .name("pendesk-camera-window".into())
            .spawn(move || window_loop(receiver, ready_sender, thread_closing))
            .map_err(|error| error.to_string())?;
        if let Err(error) = ready_receiver.recv().map_err(|error| error.to_string())? {
            let _ = thread.join();
            return Err(error);
        }
        Ok(Self {
            sender,
            closing,
            thread: Some(thread),
        })
    }

    pub fn frame(&self, frame: Frame) -> bool {
        match self.sender.try_send(frame) {
            Ok(()) | Err(TrySendError::Full(_)) => true,
            Err(TrySendError::Disconnected(_)) => false,
        }
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        self.closing.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn window_loop(
    receiver: Receiver<Frame>,
    ready: mpsc::Sender<Result<(), String>>,
    closing: Arc<AtomicBool>,
) {
    let initialized = unsafe { initialize_window() };
    let (hwnd, mut data, factory) = match initialized {
        Ok(state) => state,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    let _ = ready.send(Ok(()));
    let mut message = MSG::default();
    let mut frames = 0;
    let mut fps = 0;
    let mut started = Instant::now();
    while !closing.load(Ordering::Relaxed) {
        while unsafe {
            windows::Win32::UI::WindowsAndMessaging::PeekMessageW(
                &mut message,
                None,
                0,
                0,
                PM_REMOVE,
            )
        }
        .as_bool()
        {
            if message.message == 0x0012 {
                closing.store(true, Ordering::Relaxed);
                break;
            }
            unsafe {
                let _ = TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        match receiver.recv_timeout(Duration::from_millis(10)) {
            Ok(mut frame) => {
                while let Ok(next) = receiver.try_recv() {
                    frame = next;
                }
                if let Ok(image) = unsafe { decode(&factory, &frame.jpeg) } {
                    if frames == 0 && fps == 0 {
                        set_title(hwnd, image.width, image.height, 0);
                        started = Instant::now();
                    }
                    frames += 1;
                    let elapsed = started.elapsed();
                    if elapsed >= Duration::from_secs(1) {
                        fps = (f64::from(frames) / elapsed.as_secs_f64()).round() as u32;
                        set_title(hwnd, image.width, image.height, fps);
                        frames = 0;
                        started = Instant::now();
                    }
                    data.image = Some(image);
                    let _ = unsafe {
                        windows::Win32::Graphics::Gdi::InvalidateRect(Some(hwnd), None, false)
                    };
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
            Err(RecvTimeoutError::Timeout) => {}
        }
    }
    unsafe {
        let _ = windows::Win32::UI::WindowsAndMessaging::DestroyWindow(hwnd);
        CoUninitialize();
    }
}

fn set_title(hwnd: HWND, width: u32, height: u32, fps: u32) {
    let title = if fps == 0 {
        format!("PenDeskCam {width}x{height}")
    } else {
        format!("PenDeskCam {width}x{height}@{fps}fps")
    };
    let title: Vec<u16> = title.encode_utf16().chain([0]).collect();
    unsafe {
        let _ = SetWindowTextW(hwnd, PCWSTR(title.as_ptr()));
    }
}

unsafe fn initialize_window() -> Result<(HWND, Box<WindowData>, IWICImagingFactory), String> {
    unsafe { CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok() }
        .map_err(|error| error.to_string())?;
    let result = (|| unsafe {
        let class = WNDCLASSW {
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(window_proc),
            hCursor: LoadCursorW(None, IDC_ARROW).map_err(|error| error.to_string())?,
            lpszClassName: w!("PenDeskRemoteCamera"),
            ..Default::default()
        };
        if RegisterClassW(&class) == 0 {
            return Err("could not register camera window".into());
        }
        let factory: IWICImagingFactory =
            CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER)
                .map_err(|error| error.to_string())?;
        let mut data = Box::new(WindowData { image: None });
        let hwnd = CreateWindowExW(
            Default::default(),
            w!("PenDeskRemoteCamera"),
            w!("PenDeskCam"),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100,
            100,
            1280,
            720,
            None,
            None,
            None,
            Some((&mut *data as *mut WindowData).cast()),
        )
        .map_err(|error| error.to_string())?;
        let _ = ShowWindow(hwnd, SW_SHOW);
        Ok((hwnd, data, factory))
    })();
    if result.is_err() {
        unsafe { CoUninitialize() };
    }
    result
}

unsafe extern "system" fn window_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if message == WM_NCCREATE {
        let create = unsafe { &*(lparam.0 as *const CREATESTRUCTW) };
        unsafe { SetWindowLongPtrW(hwnd, GWLP_USERDATA, create.lpCreateParams as isize) };
    }
    if message == WM_PAINT {
        let mut paint = PAINTSTRUCT::default();
        let hdc = unsafe { BeginPaint(hwnd, &mut paint) };
        let mut client = RECT::default();
        let _ = unsafe { GetClientRect(hwnd, &mut client) };
        let client_width = client.right - client.left;
        let client_height = client.bottom - client.top;
        let _ = unsafe { PatBlt(hdc, 0, 0, client_width, client_height, BLACKNESS) };
        let pointer = unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) };
        if pointer != 0 && client_width > 0 && client_height > 0 {
            let data = unsafe { &*(pointer as *const WindowData) };
            if let Some(image) = data.image.as_ref() {
                let scale = (f64::from(client_width) / f64::from(image.width))
                    .min(f64::from(client_height) / f64::from(image.height));
                let width = (f64::from(image.width) * scale).round() as i32;
                let height = (f64::from(image.height) * scale).round() as i32;
                let bitmap = BITMAPINFO {
                    bmiHeader: BITMAPINFOHEADER {
                        biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
                        biWidth: image.width as i32,
                        biHeight: -(image.height as i32),
                        biPlanes: 1,
                        biBitCount: 32,
                        biCompression: BI_RGB.0,
                        ..Default::default()
                    },
                    ..Default::default()
                };
                unsafe {
                    StretchDIBits(
                        hdc,
                        (client_width - width) / 2,
                        (client_height - height) / 2,
                        width,
                        height,
                        0,
                        0,
                        image.width as i32,
                        image.height as i32,
                        Some(image.pixels.as_ptr().cast()),
                        &bitmap,
                        DIB_RGB_COLORS,
                        SRCCOPY,
                    );
                }
            }
        }
        let _ = unsafe { EndPaint(hwnd, &paint) };
        return LRESULT(0);
    }
    if message == WM_ERASEBKGND {
        return LRESULT(1);
    }
    if message == WM_DESTROY {
        unsafe { PostQuitMessage(0) };
        return LRESULT(0);
    }
    unsafe { DefWindowProcW(hwnd, message, wparam, lparam) }
}

unsafe fn decode(factory: &IWICImagingFactory, jpeg: &[u8]) -> Result<Image, String> {
    unsafe {
        let stream = factory.CreateStream().map_err(|error| error.to_string())?;
        stream
            .InitializeFromMemory(jpeg)
            .map_err(|error| error.to_string())?;
        let decoder = factory
            .CreateDecoderFromStream(&stream, std::ptr::null(), WICDecodeMetadataCacheOnLoad)
            .map_err(|error| error.to_string())?;
        let frame = decoder.GetFrame(0).map_err(|error| error.to_string())?;
        let converter = factory
            .CreateFormatConverter()
            .map_err(|error| error.to_string())?;
        converter
            .Initialize(
                &frame,
                &GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone,
                None,
                0.0,
                WICBitmapPaletteTypeCustom,
            )
            .map_err(|error| error.to_string())?;
        let mut width = 0;
        let mut height = 0;
        converter
            .GetSize(&mut width, &mut height)
            .map_err(|error| error.to_string())?;
        let mut pixels = vec![0; width as usize * height as usize * 4];
        converter
            .CopyPixels(std::ptr::null(), width * 4, &mut pixels)
            .map_err(|error| error.to_string())?;
        Ok(Image {
            width,
            height,
            pixels,
        })
    }
}
