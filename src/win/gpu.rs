use windows::Win32::Graphics::Direct3D::{D3D_DRIVER_TYPE_HARDWARE, D3D_FEATURE_LEVEL_11_1};
use windows::Win32::Graphics::Direct3D11::{
    D3D11CreateDevice, D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
    D3D11_SDK_VERSION, ID3D11Device, ID3D11DeviceContext, ID3D11Multithread,
};
use windows::Win32::Graphics::Dxgi::IDXGIDevice;
use windows::core::{Interface, Result};
pub struct Gpu {
    pub device: ID3D11Device,
    pub context: ID3D11DeviceContext,
}
impl Gpu {
    pub fn new() -> Result<Self> {
        unsafe {
            let mut device = None;
            let mut context = None;
            D3D11CreateDevice(
                None,
                D3D_DRIVER_TYPE_HARDWARE,
                windows::Win32::Foundation::HMODULE::default(),
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                Some(&[D3D_FEATURE_LEVEL_11_1]),
                D3D11_SDK_VERSION,
                Some(&mut device),
                None,
                Some(&mut context),
            )?;
            let device = device.unwrap();
            let context = context.unwrap();
            let multithread: ID3D11Multithread = device.cast()?;
            let _ = multithread.SetMultithreadProtected(true);
            Ok(Self { device, context })
        }
    }
    pub fn dxgi(&self) -> Result<IDXGIDevice> {
        self.device.cast()
    }
}
