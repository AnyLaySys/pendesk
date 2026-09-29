use std::mem::size_of;
use std::sync::OnceLock;
use windows::Win32::Foundation::{POINT, RECT};
use windows::Win32::UI::Controls::{
    CreateSyntheticPointerDevice, HSYNTHETICPOINTERDEVICE, POINTER_FEEDBACK_DEFAULT,
    POINTER_TYPE_INFO, POINTER_TYPE_INFO_0,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    INPUT, INPUT_0, INPUT_KEYBOARD, INPUT_MOUSE, KEYBDINPUT, KEYEVENTF_KEYUP, MOUSEEVENTF_ABSOLUTE,
    MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP,
    MOUSEEVENTF_MOVE, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_VIRTUALDESK,
    MOUSEEVENTF_WHEEL, MOUSEINPUT, SendInput, VIRTUAL_KEY,
};
use windows::Win32::UI::Input::Pointer::{
    InjectSyntheticPointerInput, POINTER_FLAG_DOWN, POINTER_FLAG_INCONTACT, POINTER_FLAG_INRANGE,
    POINTER_FLAG_NEW, POINTER_FLAG_PRIMARY, POINTER_FLAG_UP, POINTER_FLAG_UPDATE, POINTER_INFO,
    POINTER_TOUCH_INFO,
};
use windows::Win32::UI::WindowsAndMessaging::{PT_TOUCH, TOUCH_MASK_CONTACTAREA};
static TOUCH_DEVICE: OnceLock<Result<isize, String>> = OnceLock::new();
#[derive(Clone, Copy)]
pub struct TouchContact {
    pub id: u8,
    pub phase: u8,
    pub x: i32,
    pub y: i32,
}
pub fn move_pointer(x: u16, y: u16) -> Result<(), String> {
    send(INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 {
            mi: MOUSEINPUT {
                dx: i32::from(x),
                dy: i32::from(y),
                dwFlags: MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                ..Default::default()
            },
        },
    })
}
pub fn touch(contacts: &[TouchContact]) -> Result<(), String> {
    if contacts.is_empty() {
        return Ok(());
    }
    let device = *TOUCH_DEVICE
        .get_or_init(|| unsafe {
            CreateSyntheticPointerDevice(PT_TOUCH, 10, POINTER_FEEDBACK_DEFAULT)
                .map(|device| device.0 as isize)
                .map_err(|error| error.to_string())
        })
        .as_ref()
        .map_err(String::clone)?;
    let primary = contacts
        .iter()
        .find(|contact| contact.phase != 3)
        .map(|contact| contact.id);
    let pointers = contacts
        .iter()
        .map(|contact| {
            let point = POINT { x: contact.x, y: contact.y };
            let mut flags = match contact.phase {
                1 => {
                    POINTER_FLAG_DOWN
                        | POINTER_FLAG_NEW
                        | POINTER_FLAG_INRANGE
                        | POINTER_FLAG_INCONTACT
                }
                2 => POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,
                _ => POINTER_FLAG_UP | POINTER_FLAG_INRANGE,
            };
            if primary == Some(contact.id) {
                flags |= POINTER_FLAG_PRIMARY;
            }
            let area = RECT {
                left: contact.x.saturating_sub(2),
                top: contact.y.saturating_sub(2),
                right: contact.x.saturating_add(2),
                bottom: contact.y.saturating_add(2),
            };
            POINTER_TYPE_INFO {
                r#type: PT_TOUCH,
                Anonymous: POINTER_TYPE_INFO_0 {
                    touchInfo: POINTER_TOUCH_INFO {
                        pointerInfo: POINTER_INFO {
                            pointerType: PT_TOUCH,
                            pointerId: u32::from(contact.id) + 1,
                            pointerFlags: flags,
                            ptPixelLocation: point,
                            ptPixelLocationRaw: point,
                            ..Default::default()
                        },
                        touchFlags: 0,
                        touchMask: TOUCH_MASK_CONTACTAREA,
                        rcContact: area,
                        rcContactRaw: area,
                        orientation: 0,
                        pressure: 0,
                        ..Default::default()
                    },
                },
            }
        })
        .collect::<Vec<_>>();
    unsafe { InjectSyntheticPointerInput(HSYNTHETICPOINTERDEVICE(device as *mut _), &pointers) }
        .map_err(|error| error.to_string())
}
pub fn button(button: u8, down: bool) -> Result<(), String> {
    let flags = match (button, down) {
        (1, true) => MOUSEEVENTF_LEFTDOWN,
        (1, false) => MOUSEEVENTF_LEFTUP,
        (2, true) => MOUSEEVENTF_RIGHTDOWN,
        (2, false) => MOUSEEVENTF_RIGHTUP,
        (3, true) => MOUSEEVENTF_MIDDLEDOWN,
        (3, false) => MOUSEEVENTF_MIDDLEUP,
        _ => return Err("invalid mouse button".into()),
    };
    send(INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 {
            mi: MOUSEINPUT {
                dwFlags: flags,
                ..Default::default()
            },
        },
    })
}
pub fn key(key: u16, down: bool) -> Result<(), String> {
    send(INPUT {
        r#type: INPUT_KEYBOARD,
        Anonymous: INPUT_0 {
            ki: KEYBDINPUT {
                wVk: VIRTUAL_KEY(key),
                dwFlags: if down {
                    Default::default()
                } else {
                    KEYEVENTF_KEYUP
                },
                ..Default::default()
            },
        },
    })
}
pub fn wheel(delta: i16) -> Result<(), String> {
    send(INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 {
            mi: MOUSEINPUT {
                mouseData: i32::from(delta) as u32,
                dwFlags: MOUSEEVENTF_WHEEL,
                ..Default::default()
            },
        },
    })
}
fn send(input: INPUT) -> Result<(), String> {
    if unsafe { SendInput(&[input], size_of::<INPUT>() as i32) } == 1 {
        Ok(())
    } else {
        Err("Windows rejected the input".into())
    }
}
