use crate::screen::Frame;
use windows::Win32::Foundation::E_INVALIDARG;
use windows::Win32::Graphics::Imaging::{
    CLSID_WICImagingFactory, GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR,
    IWICImagingFactory, WICBitmapEncoderNoCache,
};
use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, CoCreateInstance, STGC_DEFAULT, STREAM_SEEK_END, STREAM_SEEK_SET,
};
use windows::Win32::System::Com::StructuredStorage::{CreateStreamOnHGlobal, IPropertyBag2, PROPBAG2};
use windows::Win32::System::Ole::PROPBAG2_TYPE_DATA;
use windows::Win32::System::Variant::{VARIANT, VT_R4};
use windows::core::{Error, Result};
pub struct Jpeg {
    factory: IWICImagingFactory,
    quality: f32,
}
impl Jpeg {
    pub fn new(quality: u32) -> Result<Self> {
        let factory: IWICImagingFactory =
            unsafe { CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER)? };
        Ok(Self {
            factory,
            quality: (quality.clamp(1, 100) as f32) / 100.0,
        })
    }
    pub fn encode(&mut self, frame: &Frame, output: &mut Vec<u8>) -> Result<()> {
        if frame.width == 0 || frame.height == 0 {
            return Err(Error::from_hresult(E_INVALIDARG));
        }
        output.clear();
        unsafe {
            let stream = CreateStreamOnHGlobal(windows::Win32::Foundation::HGLOBAL(std::ptr::null_mut()), true)?;
            let encoder = self.factory.CreateEncoder(&GUID_ContainerFormatJpeg, std::ptr::null())?;
            encoder.Initialize(&stream, WICBitmapEncoderNoCache)?;
            let mut frame_encode = None;
            let mut options: Option<IPropertyBag2> = None;
            encoder.CreateNewFrame(&mut frame_encode, &mut options)?;
            let frame_encode = frame_encode.unwrap();
            if let Some(options) = options.as_ref() {
                let mut name: Vec<u16> = "ImageQuality\0".encode_utf16().collect();
                let property = PROPBAG2 {
                    dwType: PROPBAG2_TYPE_DATA.0 as u32,
                    vt: VT_R4,
                    cfType: 0,
                    dwHint: 0,
                    pstrName: windows::core::PWSTR(name.as_mut_ptr()),
                    clsid: windows::core::GUID::zeroed(),
                };
                let value = VARIANT::from(self.quality);
                let _ = options.Write(1, &property, &value);
                frame_encode.Initialize(options)?;
            } else {
                frame_encode.Initialize(None)?;
            }
            frame_encode.SetSize(frame.width, frame.height)?;
            let mut format = GUID_WICPixelFormat24bppBGR;
            frame_encode.SetPixelFormat(&mut format)?;
            if format != GUID_WICPixelFormat24bppBGR {
                return Err(Error::from_hresult(E_INVALIDARG));
            }
            frame_encode.WritePixels(frame.height, frame.stride, frame.pixels)?;
            frame_encode.Commit()?;
            encoder.Commit()?;
            let mut size = 0;
            stream.Seek(0, STREAM_SEEK_END, Some(&mut size))?;
            stream.Seek(0, STREAM_SEEK_SET, None)?;
            output.resize(size as usize, 0);
            let mut read = 0;
            stream
                .Read(output.as_mut_ptr().cast(), size as u32, Some(&mut read))
                .ok()?;
            output.truncate(read as usize);
            let _ = stream.Commit(STGC_DEFAULT);
        }
        Ok(())
    }
}
