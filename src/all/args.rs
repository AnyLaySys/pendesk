#[derive(Clone)]
pub struct Config {
    pub fps: u32,
    pub quality: u32,
    pub port: u16,
    pub token: [u8; 32],
}
pub enum Command {
    Run { config: Option<Config>, wait: bool },
    Restart,
    Stop,
    Startup(bool),
    Help,
}
pub fn parse() -> Result<Command, String> {
    let mut arguments = std::env::args().skip(1);
    let Some(option) = arguments.next() else {
        return Ok(Command::Run {
            config: None,
            wait: false,
        });
    };
    match option.as_str() {
        "run" if arguments.next().is_none() => Ok(Command::Run {
            config: None,
            wait: true,
        }),
        "restart" if arguments.next().is_none() => Ok(Command::Restart),
        "stop" if arguments.next().is_none() => Ok(Command::Stop),
        "+startup" if arguments.next().is_none() => Ok(Command::Startup(true)),
        "-startup" if arguments.next().is_none() => Ok(Command::Startup(false)),
        "--help" | "-h" if arguments.next().is_none() => Ok(Command::Help),
        _ => parse_config(std::iter::once(option).chain(arguments)).map(|config| Command::Run {
            config: Some(config),
            wait: false,
        }),
    }
}
pub fn stored(text: &str) -> Result<Config, String> {
    let mut port = None;
    let mut pairing_token = None;
    let mut fps = 30;
    let mut quality = 80;
    for line in text.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        match key.trim() {
            "port" => port = Some(number(value.trim(), "port")?),
            "token" => pairing_token = Some(token(value.trim())?),
            "fps" => fps = number(value.trim(), "fps")?,
            "quality" => quality = number(value.trim(), "quality")?,
            _ => {}
        }
    }
    let port = port.ok_or_else(|| "active-config has no port".to_string())?;
    if port == 0 {
        return Err("active-config has an invalid port".into());
    }
    if !(1..=60).contains(&fps) {
        return Err("active-config fps must be from 1 to 60".into());
    }
    if !(1..=100).contains(&quality) {
        return Err("active-config quality must be from 1 to 100".into());
    }
    Ok(Config {
        fps,
        quality,
        port,
        token: pairing_token.ok_or_else(|| "active-config has no token".to_string())?,
    })
}
fn parse_config(mut arguments: impl Iterator<Item = String>) -> Result<Config, String> {
    let mut fps = 60;
    let mut quality = 80;
    let mut port = 7193;
    let mut pairing_token = None;
    while let Some(option) = arguments.next() {
        match option.as_str() {
            "--fps" => fps = number(&next(&mut arguments, "--fps")?, "--fps")?,
            "--quality" => quality = number(&next(&mut arguments, "--quality")?, "--quality")?,
            "--port" => port = number(&next(&mut arguments, "--port")?, "--port")?,
            "--token" => pairing_token = Some(token(&next(&mut arguments, "--token")?)?),
            "--help" | "-h" => return Err(usage()),
            _ => return Err(usage()),
        }
    }
    if !(1..=60).contains(&fps) {
        return Err("--fps must be from 1 to 60".into());
    }
    if port == 0 {
        return Err("--port must be from 1 to 65535".into());
    }
    if !(1..=100).contains(&quality) {
        return Err("--quality must be from 1 to 100".into());
    }
    Ok(Config {
        fps,
        quality,
        port,
        token: pairing_token.ok_or_else(|| "--token is required".to_string())?,
    })
}
fn next(arguments: &mut impl Iterator<Item = String>, option: &str) -> Result<String, String> {
    arguments
        .next()
        .ok_or_else(|| format!("{option} needs a value"))
}
fn number<T: std::str::FromStr>(value: &str, option: &str) -> Result<T, String> {
    value
        .parse()
        .map_err(|_| format!("{option} has an invalid value"))
}
fn token(value: &str) -> Result<[u8; 32], String> {
    if value.len() != 64 {
        return Err("--token must be 64 hexadecimal characters".into());
    }
    let mut output = [0; 32];
    for (index, pair) in value.as_bytes().as_chunks::<2>().0.iter().enumerate() {
        output[index] = nibble(pair[0])
            .zip(nibble(pair[1]))
            .map(|(high, low)| high << 4 | low)
            .ok_or_else(|| "--token must be hexadecimal".to_string())?;
    }
    Ok(output)
}
fn nibble(value: u8) -> Option<u8> {
    match value {
        b'0'..=b'9' => Some(value - b'0'),
        b'a'..=b'f' => Some(value - b'a' + 10),
        b'A'..=b'F' => Some(value - b'A' + 10),
        _ => None,
    }
}
pub fn usage() -> String {
    "Usage: pendesk [--token <64-hex> [--port <1-65535>] [--fps <1-60>] [--quality <1-100>]]\n       pendesk restart|stop|+startup|-startup".into()
}
