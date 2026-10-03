use crate::all::APP_ID;
use crate::all::cmd::{self as cmd, Setup};
use crate::{elevation, startup};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::thread;
use std::time::{Duration, Instant};
use windows::Win32::Security::Cryptography::{BCRYPT_USE_SYSTEM_PREFERRED_RNG, BCryptGenRandom};

struct App {
    start: String,
    data: String,
    slot: String,
}

pub fn install(serial: Option<String>) -> Result<(), String> {
    let serial = confirm_device(serial)?;
    let package = std::env::current_exe()
        .map_err(|_| "路径文件缺失".to_string())?
        .parent()
        .ok_or_else(|| "路径文件缺失".to_string())?
        .join("pendesk.amr");
    if !package.is_file() {
        return Err("路径文件缺失".into());
    }
    let previous = installed(&serial, true)?;
    if let Some(app) = previous {
        pen_shell(
            &serial,
            &format!(
                "if [ ! -f {data}/config ] && [ -s {slot}/config ]; then mkdir -p {data}; cp {slot}/config {data}/config; chmod 600 {data}/config; fi",
                data = sh_quote(&app.data),
                slot = sh_quote(&app.slot)
            ),
        )?;
    }
    pen_shell(
        &serial,
        "touch /userdata/.disable_app_whitelist_clean && sync && test -f /userdata/.disable_app_whitelist_clean",
    )?;
    let remote = format!("/tmp/pendesk-{}.amr", std::process::id());
    push(&serial, &package, &remote)?;
    let result = pen_shell(
        &serial,
        &format!("/usr/bin/miniapp_cli install {}", sh_quote(&remote)),
    );
    let _ = pen_shell(&serial, &format!("rm -f {}", sh_quote(&remote)));
    let result = result?;
    if !result.contains("\"ret\":0") && !result.contains("\"ret\": 0") {
        std::process::exit(1);
    }
    if let Some(app) = installed(&serial, false)? {
        pen_shell(
            &serial,
            &format!("/bin/sh {} restart", sh_quote(&app.start)),
        )?;
    }
    println!("已安装PenDesk");
    Ok(())
}

pub fn configure(setup: Setup) -> Result<(), String> {
    let serial = confirm_device(setup.serial.clone())?;
    let app = installed(&serial, false)?.ok_or_else(|| "未安装PenDesk".to_string())?;
    let tailscale = tailscale_path()?;
    let local = tailscale_ip(&tailscale)?;
    let host = setup.host.unwrap_or_else(|| local.clone());
    if !tailnet_ip(&host) {
        return Err("HostAddress须为Tailnet IPv4".into());
    }
    let local_device = host == local;
    if local_device && !tailscale_running(&tailscale) {
        return Err("Tailscale未登录".into());
    }
    let config_path = config_path();
    let previous = if local_device {
        fs::read_to_string(&config_path).unwrap_or_default()
    } else {
        String::new()
    };
    let token = setup
        .token
        .or_else(|| value(&previous, "token"))
        .unwrap_or(random_token()?);
    if !cmd::valid_token(&token) {
        return Err("Token格式错误".into());
    }
    let config = format!(
        "host={host}\nport={port}\nsocks=127.0.0.1:1055\ntoken={token}\n",
        port = setup.port
    );
    let temporary = std::env::temp_dir().join(format!("pendesk-{}-config", std::process::id()));
    fs::write(&temporary, config.as_bytes()).map_err(|_| "路径提权失败".to_string())?;
    let result = configure_pen(&serial, &app, &temporary, &config);
    let _ = fs::remove_file(&temporary);
    result?;
    println!("直连>DERP");
    login(&serial, &app, setup.authkey.as_deref())?;
    if local_device {
        fs::create_dir_all(config_path.parent().unwrap())
            .map_err(|_| "路径提权失败".to_string())?;
        fs::write(&config_path, config.as_bytes()).map_err(|_| "路径提权失败".to_string())?;
        startup::set(true).map_err(|_| "配置自启失败".to_string())?;
        elevation::launch(["run".into()]).map_err(|_| "启动PenDesk失败".to_string())?;
    }
    println!("设备:{host}");
    if local_device {
        println!("配置:{}", config_path.display());
    }
    Ok(())
}

fn configure_pen(serial: &str, app: &App, temporary: &Path, config: &str) -> Result<(), String> {
    pen_shell(
        serial,
        &format!(
            "mkdir -p {}; chmod 700 {}",
            sh_quote(&app.data),
            sh_quote(&app.data)
        ),
    )?;
    push(serial, temporary, &format!("{}/config.new", app.data))?;
    pen_shell(
        serial,
        &format!("/bin/sh {} configure", sh_quote(&app.start)),
    )?;
    if !config.contains("host=") {
        return Err("设备命令失败".into());
    }
    Ok(())
}

fn login(serial: &str, app: &App, authkey: Option<&str>) -> Result<(), String> {
    pen_shell(serial, &format!("/bin/sh {} network", sh_quote(&app.start)))?;
    if let Some(authkey) = authkey {
        let temporary = std::env::temp_dir().join(format!("pendesk-{}-auth", std::process::id()));
        fs::write(&temporary, authkey).map_err(|_| "路径提权失败".to_string())?;
        let result = push(serial, &temporary, &format!("{}/auth", app.data));
        let _ = fs::remove_file(&temporary);
        result?;
        pen_shell(
            serial,
            &format!(
                "chmod 600 {data}/auth; /bin/sh {start} login",
                data = sh_quote(&app.data),
                start = sh_quote(&app.start)
            ),
        )?;
        let _ = pen_shell(serial, &format!("rm -f {}/auth", sh_quote(&app.data)));
    } else {
        pen_shell(serial, &format!("/bin/sh {} login", sh_quote(&app.start)))?;
    }
    let status = pen_shell(
        serial,
        &format!(
            "/bin/sh {} status | sed -n -e '/\"BackendState\"/s/.*\"BackendState\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/STATE:\\1/p' -e '/\"AuthURL\"/s/.*\"AuthURL\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/AUTH:\\1/p'",
            sh_quote(&app.start)
        ),
    )?;
    if status.contains("STATE:Running") {
        println!("Tailscale:Running");
    } else if let Some(url) = status
        .lines()
        .find_map(|line| line.strip_prefix("AUTH:").filter(|url| !url.is_empty()))
    {
        println!("前往 {url} 授权");
        println!("授权后自动重连");
    } else {
        println!("未授权/无网络");
    }
    Ok(())
}

fn confirm_device(serial: Option<String>) -> Result<String, String> {
    let output = adb(vec!["devices".into()])?;
    let devices = String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter_map(|line| {
            let mut fields = line.split_whitespace();
            match (fields.next(), fields.next()) {
                (Some(name), Some("device")) => Some(name.to_string()),
                _ => None,
            }
        })
        .collect::<Vec<_>>();
    match serial {
        Some(serial) if devices.iter().any(|device| device == &serial) => Ok(serial),
        Some(_) => Err("指定ADB设备未连接".into()),
        None if devices.is_empty() => Err("无ADB设备".into()),
        None if devices.len() > 1 => Err("用-serial指定设备".into()),
        None => Ok(devices[0].clone()),
    }
}

fn installed(serial: &str, optional: bool) -> Result<Option<App>, String> {
    let script = format!(
        "latest=; for file in /userdisk/*/data/mini_app/pkg/{APP_ID}/*/bin/run /userdisk/*/*/data/mini_app/pkg/{APP_ID}/*/bin/run /userdata/*/data/mini_app/pkg/{APP_ID}/*/bin/run /userdata/*/*/data/mini_app/pkg/{APP_ID}/*/bin/run; do [ -f \"$file\" ] || continue; if [ -z \"$latest\" ] || [ \"$file\" -nt \"$latest\" ]; then latest=$file; fi; done; printf \"%s\" \"$latest\""
    );
    let start = pen_shell(serial, &script)?;
    if start.is_empty() && optional {
        return Ok(None);
    }
    if !start.starts_with("/userdisk/") && !start.starts_with("/userdata/") {
        return Err("未安装PenDesk".into());
    }
    let suffix = "/bin/run";
    if !start.ends_with(suffix) {
        return Err("未安装PenDesk".into());
    }
    let slot = start.trim_end_matches(suffix).to_string();
    let package = slot
        .rsplit_once('/')
        .map(|(value, _)| value)
        .unwrap_or(&slot);
    Ok(Some(App {
        start,
        data: format!("{package}/data"),
        slot,
    }))
}

fn tailscale_path() -> Result<PathBuf, String> {
    std::env::var_os("ProgramFiles")
        .map(PathBuf::from)
        .map(|path| path.join("Tailscale").join("tailscale.exe"))
        .filter(|path| path.is_file())
        .or_else(|| {
            Command::new("tailscale")
                .arg("--version")
                .output()
                .ok()
                .map(|_| PathBuf::from("tailscale"))
        })
        .ok_or_else(|| "Tailscale未安装".into())
}

fn tailscale_ip(path: &Path) -> Result<String, String> {
    let output = process(path, vec!["ip".into(), "-4".into()], Duration::from_secs(5))?;
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(str::trim)
        .find(|line| tailnet_ip(line))
        .map(str::to_string)
        .ok_or_else(|| "Tailscale未登录".into())
}

fn tailscale_running(path: &Path) -> bool {
    process(
        path,
        vec!["status".into(), "--json".into()],
        Duration::from_secs(5),
    )
    .map(|output| {
        String::from_utf8_lossy(&output.stdout)
            .replace(' ', "")
            .contains("\"BackendState\":\"Running\"")
    })
    .unwrap_or(false)
}

fn push(serial: &str, local: &Path, remote: &str) -> Result<(), String> {
    adb(vec![
        "-s".into(),
        serial.into(),
        "push".into(),
        local.to_string_lossy().into_owned(),
        remote.into(),
    ])?;
    Ok(())
}

fn adb(arguments: Vec<String>) -> Result<Output, String> {
    let output = Command::new("adb")
        .args(arguments)
        .output()
        .map_err(|error| error.to_string())?;
    print!("{}", String::from_utf8_lossy(&output.stdout));
    if !output.stdout.is_empty() && !output.stdout.ends_with(b"\n") {
        println!();
    }
    eprint!("{}", String::from_utf8_lossy(&output.stderr));
    if !output.stderr.is_empty() && !output.stderr.ends_with(b"\n") {
        eprintln!();
    }
    if !output.status.success() {
        std::process::exit(output.status.code().unwrap_or(1));
    }
    Ok(output)
}

fn pen_shell(serial: &str, command: &str) -> Result<String, String> {
    let output = adb(vec![
        "-s".into(),
        serial.into(),
        "shell".into(),
        command.into(),
    ])?;
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

fn process(path: &Path, arguments: Vec<String>, timeout: Duration) -> Result<Output, String> {
    let mut child = Command::new(path)
        .args(arguments)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|_| "命令执行失败".to_string())?;
    let deadline = Instant::now() + timeout;
    loop {
        if child
            .try_wait()
            .map_err(|_| "命令执行失败".to_string())?
            .is_some()
        {
            return child.wait_with_output().map_err(|_| "命令执行失败".into());
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            return Err("命令超时".into());
        }
        thread::sleep(Duration::from_millis(50));
    }
}

fn random_token() -> Result<String, String> {
    let mut bytes = [0u8; 32];
    let status = unsafe { BCryptGenRandom(None, &mut bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) };
    if status.0 != 0 {
        return Err("Token格式错误".into());
    }
    Ok(bytes.iter().map(|byte| format!("{byte:02x}")).collect())
}

fn tailnet_ip(value: &str) -> bool {
    let parts = value.split('.').collect::<Vec<_>>();
    if parts.len() != 4 || parts.iter().any(|part| part.parse::<u8>().is_err()) {
        return false;
    }
    parts[0] == "100"
        && parts[1]
            .parse::<u8>()
            .map(|value| (64..=127).contains(&value))
            .unwrap_or(false)
}

fn value(text: &str, key: &str) -> Option<String> {
    text.lines()
        .find_map(|line| line.strip_prefix(&format!("{key}=")).map(str::to_string))
}

fn config_path() -> PathBuf {
    std::env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .join("cfg/DevCfg")
}

fn sh_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}
