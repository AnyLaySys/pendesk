use crate::all::protocol::TOUCH_POINTS;
use std::mem::size_of;
use std::time::{Duration, Instant};
use windows::Win32::Foundation::{ERROR_NOT_READY, POINT, RECT};
use windows::Win32::UI::Controls::{
    CreateSyntheticPointerDevice, DestroySyntheticPointerDevice, HSYNTHETICPOINTERDEVICE,
    POINTER_FEEDBACK_DEFAULT, POINTER_TYPE_INFO, POINTER_TYPE_INFO_0,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    INPUT, INPUT_0, INPUT_KEYBOARD, INPUT_MOUSE, KEYBDINPUT, KEYEVENTF_KEYUP, MOUSEEVENTF_ABSOLUTE,
    MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP,
    MOUSEEVENTF_MOVE, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_VIRTUALDESK,
    MOUSEEVENTF_WHEEL, MOUSEINPUT, SendInput, VIRTUAL_KEY,
};
use windows::Win32::UI::Input::Pointer::{
    InjectSyntheticPointerInput, POINTER_FLAG_CANCELED, POINTER_FLAG_DOWN, POINTER_FLAG_INCONTACT,
    POINTER_FLAG_INRANGE, POINTER_FLAG_UP, POINTER_FLAG_UPDATE, POINTER_INFO, POINTER_TOUCH_INFO,
};
use windows::Win32::UI::WindowsAndMessaging::{PT_TOUCH, TOUCH_MASK_CONTACTAREA};
use windows::core::HRESULT;
#[derive(Clone, Copy, Default)]
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
#[derive(Default)]
pub struct Touch {
    device: Option<HSYNTHETICPOINTERDEVICE>,
    active: [Option<TouchContact>; TOUCH_POINTS],
    updated: Option<Instant>,
}

impl Touch {
    pub fn inject(&mut self, contacts: &[TouchContact]) -> Result<(), String> {
        if contacts.is_empty() {
            return Ok(());
        }
        if contacts.len() > TOUCH_POINTS {
            return Err("too many touch points".into());
        }
        if self.device.is_none() {
            self.device = Some(
                unsafe {
                    CreateSyntheticPointerDevice(
                        PT_TOUCH,
                        TOUCH_POINTS as u32,
                        POINTER_FEEDBACK_DEFAULT,
                    )
                }
                .map_err(|error| error.to_string())?,
            );
        }
        let mut pointers = [POINTER_TYPE_INFO::default(); TOUCH_POINTS];
        let mut next = self.active;
        for (pointer, contact) in pointers.iter_mut().zip(contacts) {
            let slot = next
                .get_mut(usize::from(contact.id))
                .ok_or("invalid touch id")?;
            let position = if contact.phase >= 3 {
                slot.unwrap_or(*contact)
            } else {
                *contact
            };
            let point = POINT {
                x: position.x,
                y: position.y,
            };
            let flags = match contact.phase {
                1 => POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,
                2 => POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,
                3 => POINTER_FLAG_UP,
                _ => POINTER_FLAG_UP | POINTER_FLAG_CANCELED,
            };
            let area = RECT {
                left: position.x.saturating_sub(2),
                top: position.y.saturating_sub(2),
                right: position.x.saturating_add(2),
                bottom: position.y.saturating_add(2),
            };
            *pointer = POINTER_TYPE_INFO {
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
                        touchMask: TOUCH_MASK_CONTACTAREA,
                        rcContact: area,
                        rcContactRaw: area,
                        ..Default::default()
                    },
                },
            };
            *slot = if contact.phase >= 3 {
                None
            } else {
                Some(*contact)
            };
        }
        let deadline = Instant::now() + Duration::from_millis(2);
        loop {
            match unsafe {
                InjectSyntheticPointerInput(self.device.unwrap(), &pointers[..contacts.len()])
            } {
                Ok(()) => {
                    self.active = next;
                    self.updated = Some(Instant::now());
                    return Ok(());
                }
                Err(error)
                    if error.code() == HRESULT::from_win32(ERROR_NOT_READY.0)
                        && Instant::now() < deadline =>
                {
                    std::thread::yield_now()
                }
                Err(error) => {
                    self.reset();
                    return Err(error.to_string());
                }
            }
        }
    }

    pub fn timeout(&self) -> i32 {
        match self
            .updated
            .filter(|_| self.active.iter().any(Option::is_some))
        {
            Some(updated) => Duration::from_millis(200)
                .saturating_sub(updated.elapsed())
                .as_millis() as i32,
            None => -1,
        }
    }

    pub fn refresh(&mut self) -> Result<(), String> {
        self.update(2)
    }

    pub fn cancel(&mut self) -> Result<(), String> {
        self.update(4)
    }

    fn update(&mut self, phase: u8) -> Result<(), String> {
        let mut contacts = [TouchContact::default(); TOUCH_POINTS];
        let mut count = 0;
        for contact in self.active.iter().flatten() {
            contacts[count] = TouchContact { phase, ..*contact };
            count += 1;
        }
        self.inject(&contacts[..count])
    }

    fn reset(&mut self) {
        if let Some(device) = self.device.take() {
            unsafe { DestroySyntheticPointerDevice(device) };
        }
        self.active.fill(None);
        self.updated = None;
    }
}

impl Drop for Touch {
    fn drop(&mut self) {
        let _ = self.cancel();
        self.reset();
    }
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
