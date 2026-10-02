use crate::all::cmd::{self as cmd, Config};
pub fn stored() -> Result<Config, String> {
    let path = std::env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .join("cfg/DevCfg");
    let text =
        std::fs::read_to_string(&path).map_err(|_| format!("could not read {}", path.display()))?;
    cmd::stored(&text)
}
