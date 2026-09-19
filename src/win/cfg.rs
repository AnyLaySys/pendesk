use crate::all::args::{self, Config};
pub fn stored() -> Result<Config, String> {
    let path = std::env::var_os("LOCALAPPDATA")
        .map(std::path::PathBuf::from)
        .ok_or_else(|| "LOCALAPPDATA is not set".to_string())?
        .join("PenDesk")
        .join("active-config");
    let text =
        std::fs::read_to_string(&path).map_err(|_| format!("could not read {}", path.display()))?;
    args::stored(&text)
}
