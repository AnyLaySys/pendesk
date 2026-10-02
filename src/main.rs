mod all;
#[path = "win/audio.rs"]
mod audio;
#[path = "win/cam.rs"]
mod cam;
#[path = "win/cfg.rs"]
mod cfg;
#[path = "win/elevation.rs"]
mod elevation;
#[path = "win/encoder.rs"]
mod encoder;
#[path = "win/files.rs"]
mod files;
#[path = "win/gpu.rs"]
mod gpu;
#[path = "win/input.rs"]
mod input;
#[path = "win/media.rs"]
mod media;
#[path = "win/mic.rs"]
mod mic;
#[path = "win/runtime.rs"]
mod runtime;
#[path = "win/screen.rs"]
mod screen;
#[path = "win/server.rs"]
mod server;
#[path = "win/session.rs"]
mod session;
#[path = "win/session_audio.rs"]
mod session_audio;
#[path = "win/session_input.rs"]
mod session_input;
#[path = "win/setup.rs"]
mod setup;
#[path = "win/signal.rs"]
mod signal;
#[path = "win/startup.rs"]
mod startup;
#[path = "win/viewport.rs"]
mod viewport;
fn main() {
    unsafe {
        let _ = windows::Win32::System::Com::CoInitializeEx(
            None,
            windows::Win32::System::Com::COINIT_MULTITHREADED,
        );
        let _ = windows::Win32::UI::HiDpi::SetProcessDpiAwarenessContext(
            windows::Win32::UI::HiDpi::DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
        );
    }
    if let Err(error) = runtime::dispatch() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
