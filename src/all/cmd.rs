#[derive(Clone)]
pub struct Config {
    pub port: u16,
    pub token: [u8; 32],
}
#[derive(Clone)]
pub struct Setup {
    pub authkey: Option<String>,
    pub host: Option<String>,
    pub port: u16,
    pub serial: Option<String>,
    pub token: Option<String>,
}
pub enum Command {
    Run { config: Option<Config>, wait: bool },
    Install { serial: Option<String> },
    Configure(Setup),
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
    let (option, serial) = if option == "-s" || option == "--serial" {
        let serial = next(&mut arguments, &option)?;
        (next(&mut arguments, "command")?, Some(serial))
    } else {
        (option, None)
    };
    match option.as_str() {
        "run" if serial.is_none() && arguments.next().is_none() => Ok(Command::Run {
            config: None,
            wait: true,
        }),
        "install" if arguments.next().is_none() => Ok(Command::Install { serial }),
        "configure" => parse_setup(arguments, serial).map(Command::Configure),
        "restart" if serial.is_none() && arguments.next().is_none() => Ok(Command::Restart),
        "stop" if serial.is_none() && arguments.next().is_none() => Ok(Command::Stop),
        "+startup" if serial.is_none() && arguments.next().is_none() => Ok(Command::Startup(true)),
        "-startup" if serial.is_none() && arguments.next().is_none() => Ok(Command::Startup(false)),
        "--help" | "-h" if serial.is_none() && arguments.next().is_none() => Ok(Command::Help),
        _ if serial.is_some() => Err(usage()),
        _ => parse_config(std::iter::once(option).chain(arguments)).map(|config| Command::Run {
            config: Some(config),
            wait: false,
        }),
    }
}
pub fn stored(text: &str) -> Result<Config, String> {
    let mut port = None;
    let mut pairing_token = None;
    for line in text.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        match key.trim() {
            "port" => port = Some(number(value.trim(), "port")?),
            "token" => pairing_token = Some(token(value.trim())?),
            _ => {}
        }
    }
    let port = port.ok_or_else(|| "DevCfg has no port".to_string())?;
    if port == 0 {
        return Err("DevCfg has an invalid port".into());
    }
    Ok(Config {
        port,
        token: pairing_token.ok_or_else(|| "DevCfg has no token".to_string())?,
    })
}
fn parse_config(mut arguments: impl Iterator<Item = String>) -> Result<Config, String> {
    let mut port = 999;
    let mut pairing_token = None;
    while let Some(option) = arguments.next() {
        match option.as_str() {
            "--port" => port = number(&next(&mut arguments, "--port")?, "--port")?,
            "--token" => pairing_token = Some(token(&next(&mut arguments, "--token")?)?),
            "--help" | "-h" => return Err(usage()),
            _ => return Err(usage()),
        }
    }
    if port == 0 {
        return Err("--port must be from 1 to 65535".into());
    }
    Ok(Config {
        port,
        token: pairing_token.ok_or_else(|| "--token is required".to_string())?,
    })
}
fn parse_setup(mut arguments: impl Iterator<Item = String>, serial: Option<String>) -> Result<Setup, String> {
    let mut setup = Setup {
        authkey: None,
        host: None,
        port: 999,
        serial,
        token: None,
    };
    while let Some(option) = arguments.next() {
        let mut value = || next(&mut arguments, &option);
        match option.as_str() {
            "--auth-key" | "--authkey" => setup.authkey = Some(value()?),
            "--host" | "--hostaddress" => setup.host = Some(value()?),
            "--port" => setup.port = number(&value()?, "--port")?,
            "--token" => setup.token = Some(value()?),
            "--help" | "-h" => return Err(usage()),
            _ => return Err(usage()),
        }
    }
    if setup.port == 0 {
        return Err("--port must be from 1 to 65535".into());
    }
    if let Some(token) = &setup.token {
        token_value(token)?;
    }
    Ok(setup)
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
    token_value(value)?;
    let mut output = [0; 32];
    for (index, pair) in value.as_bytes().as_chunks::<2>().0.iter().enumerate() {
        output[index] = nibble(pair[0])
            .zip(nibble(pair[1]))
            .map(|(high, low)| high << 4 | low)
            .ok_or_else(|| "--token must be hexadecimal".to_string())?;
    }
    Ok(output)
}
pub fn valid_token(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|value| value.is_ascii_hexdigit())
}
fn token_value(value: &str) -> Result<(), String> {
    if !valid_token(value) {
        return Err("--token must be 64 hexadecimal characters".into());
    }
    Ok(())
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
    "Usage: pendesk [--token <64-hex> [--port <1-65535>]]\n       pendesk [-s|--serial <device>] install\n       pendesk [-s|--serial <device>] configure [--host <Tailnet IPv4>] [--token <64-hex>] [--auth-key <key>]\n       pendesk restart|stop|+startup|-startup".into()
}
