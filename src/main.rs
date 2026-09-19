mod all;
#[path = "win/audio.rs"]
mod audio;
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
#[path = "win/runtime.rs"]
mod runtime;
#[path = "win/screen.rs"]
mod screen;
#[path = "win/server.rs"]
mod server;
fn main() {
    unsafe {
        let _ = windows::Win32::UI::HiDpi::SetProcessDpiAwarenessContext(
            windows::Win32::UI::HiDpi::DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
        );
    }
    if let Err(error) = runtime::dispatch() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
