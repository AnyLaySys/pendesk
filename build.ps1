param(
    [Parameter(Position = 0)][ValidateSet('build', 'install', 'configure')][string]$action = 'build',
    [string]$authkey,
    [string]$hostaddress,
    [ValidateRange(1, 65535)][int]$port = 7193,
    [string]$serial,
    [string]$token
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$target = Join-Path $build 'target'; $out = Join-Path $build 'out'
$penBuild = Join-Path $out 'pen'
$penSource = Join-Path $root 'src\pen'; $miniAppSource = Join-Path $penSource 'miniapp'
$hostExecutable = Join-Path $out 'pendesk.exe'
$package = Join-Path $out 'pendesk.amr'; $state = Join-Path $env:LOCALAPPDATA 'PenDesk'
$portSpecified = $PSBoundParameters.ContainsKey('port')

function Temporary-Path([string]$Name) {
    $directory = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    $path = [IO.Path]::GetFullPath((Join-Path $directory "pendesk-$PID-$([guid]::NewGuid().ToString('N'))-$Name"))
    if (!$path.StartsWith($directory, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe temporary path' }
    $path
}

function Protect-Path([string]$Path) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    & icacls.exe $Path '/inheritance:r' '/grant:r' "*$($identity.Value):(F)" '*S-1-5-18:(F)' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not protect $Path" }
}

function Get-MiniAppManifest {
    $manifest = Get-Content -LiteralPath (Join-Path $miniAppSource 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$manifest.appid -notmatch '^\d+$') { throw 'The miniapp id is invalid' }
    $manifest
}

function ConvertTo-WSLPath([string]$Path) {
    $value = (& wsl.exe --exec wslpath -a $Path).Trim()
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $value
}

function Require([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing $Path" }
}

function TailscalePath {
    $command = Get-Command tailscale -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $path = Join-Path $env:ProgramFiles 'Tailscale\tailscale.exe'
    if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    throw 'Tailscale is not installed'
}

function Invoke-ADB([string[]]$Arguments) {
    & (Get-Command adb -ErrorAction Stop).Source @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'adb failed' }
}

function Invoke-Pen([string]$Command) {
    $result = (Invoke-ADB @('-s', $serial, 'shell', "$Command; result=`$?; echo; echo PENDESK_STATUS:`$result")) -join "`n"
    if ($result -notmatch '(?m)^PENDESK_STATUS:0\s*$') {
        if ($result -match 'shell auth|password:') { throw "Authenticate this pen first: adb -s $serial shell auth" }
        throw 'The pen command failed or did not return its success marker'
    }
    ($result -replace '(?m)^PENDESK_STATUS:0\s*$', '').Trim()
}

function Confirm-Pen {
    $devices = @(Invoke-ADB @('devices') | ForEach-Object {
        if ($_ -match '^\s*(\S+)\s+device(?:\s|$)') { $Matches[1] }
    })
    if (!$serial) {
        if ($devices.Count -ne 1) { throw 'Specify -Serial when adb has zero or multiple devices' }
        $script:serial = $devices[0]
    } elseif ($devices -notcontains $serial) { throw 'The selected adb device is not ready' }
    $ready = Invoke-Pen 'test "$(uname -m)" = aarch64 && command -v miniapp_cli >/dev/null && command -v gst-launch-1.0 >/dev/null && echo PENDESK_LINUX_PEN'
    if ($ready -ne 'PENDESK_LINUX_PEN') { throw 'The selected device is not the expected Linux dictionary pen' }
}

function Installed-App([switch]$Optional) {
    $appID = [string](Get-MiniAppManifest).appid
    $find = 'latest=; for file in /userdisk/*/data/mini_app/pkg/__APPID__/*/bin/run /userdisk/*/*/data/mini_app/pkg/__APPID__/*/bin/run /userdata/*/data/mini_app/pkg/__APPID__/*/bin/run /userdata/*/*/data/mini_app/pkg/__APPID__/*/bin/run; do [ -f "$file" ] || continue; if [ -z "$latest" ] || [ "$file" -nt "$latest" ]; then latest=$file; fi; done; printf "%s" "$latest"'
    $start = Invoke-Pen ($find.Replace('__APPID__', $appID))
    if (!$start -and $Optional) { return }
    if ($start -notmatch "^/(?:userdisk|userdata)/[A-Za-z0-9_/-]+/mini_app/pkg/$appID/[^/]+/bin/run$") { throw 'Install the generic PenDesk AMR first' }
    $package = $start -replace '/[^/]+/bin/run$', ''
    @{ start=$start; data="$package/data"; slot=($start -replace '/bin/run$', '') }
}

function Login-Pen {
    Confirm-Pen
    $app = Installed-App
    Invoke-Pen "/bin/sh '$($app.start)' network" | Out-Null
    if ($authkey) {
        $temporary = Temporary-Path 'auth'
        try {
            [IO.File]::WriteAllText($temporary, $authkey, [Text.UTF8Encoding]::new($false))
            Protect-Path $temporary
            Invoke-ADB @('-s', $serial, 'push', $temporary, "$($app.data)/auth") | Out-Null
            Invoke-Pen "chmod 600 '$($app.data)/auth'; /bin/sh '$($app.start)' login" | Out-Null
        } finally {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction Ignore
            Invoke-Pen "rm -f '$($app.data)/auth'" | Out-Null
        }
    } else {
        Invoke-Pen "/bin/sh '$($app.start)' login" | Out-Null
    }
    $status = $null
    for ($attempt = 0; $attempt -lt 12; ++$attempt) {
        $status = (Invoke-Pen "/bin/sh '$($app.start)' status") | ConvertFrom-Json
        if ($status.AuthURL -or $status.BackendState -in @('Running', 'NeedsMachineAuth')) { break }
        Start-Sleep -Seconds 1
    }
    Write-Output "Tailscale: $($status.BackendState)"
    if ($status.AuthURL) {
        Write-Output "Authorize this new device in your browser: $($status.AuthURL)"
        Write-Output 'After approval, PenDesk reconnects automatically.'
    } elseif ($status.BackendState -ne 'Running') {
        Write-Output 'Authorization or connectivity is pending. Check Wi-Fi, then run configure again.'
    }
}

function Install-Package([string]$Package) {
    Require $Package
    Confirm-Pen
    $previous = Installed-App -Optional
    if ($previous) {
        Invoke-Pen "if [ ! -f '$($previous.data)/config' ] && [ -s '$($previous.slot)/config' ]; then mkdir -p '$($previous.data)'; cp '$($previous.slot)/config' '$($previous.data)/config'; chmod 600 '$($previous.data)/config'; fi" | Out-Null
        Invoke-Pen "/bin/sh '$($previous.start)' stop" | Out-Null
    }
    Invoke-Pen 'touch /userdata/.disable_app_whitelist_clean && sync && test -f /userdata/.disable_app_whitelist_clean' | Out-Null
    $remote = "/tmp/pendesk-$PID.amr"
    Invoke-ADB @('-s', $serial, 'push', $Package, $remote) | Out-Null
    try {
        $result = Invoke-Pen "/usr/bin/miniapp_cli install '$remote'"
        if ($result -notmatch '"ret"\s*:\s*0\b') { throw 'The pen did not install the PenDesk package' }
    } finally {
        Invoke-Pen "rm -f '$remote'" | Out-Null
    }
    Write-Output 'Generic AMR installed. Existing runtime settings are retained; new devices need pairing and independent Tailscale authorization.'
}

function Build-Daemon {
    New-Item -ItemType Directory -Force $penBuild | Out-Null
    $penSourceWSL = ConvertTo-WSLPath $penSource
    $penBuildWSL = ConvertTo-WSLPath $penBuild
    & wsl.exe --exec bash --noprofile --norc -c "aarch64-linux-gnu-gcc -std=c17 -O2 -Wall -Wextra -Werror -pthread -static '$penSourceWSL/daemon.c' '$penSourceWSL/cfg.c' '$penSourceWSL/io.c' '$penSourceWSL/link.c' '$penSourceWSL/input.c' '$penSourceWSL/video.c' '$penSourceWSL/audio.c' '$penSourceWSL/preview.c' '$penSourceWSL/files.c' -o '$penBuildWSL/daemon'"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Fetch-Tailscale {
    $version = '1.102.4'
    $cache = Join-Path $env:LOCALAPPDATA 'PenDesk\tailscale-release'
    $archive = Join-Path $cache "tailscale_${version}_arm64.tgz"
    $expected = '9DD1E6A592A014BBAEA0103167FFE299ADEDA4BA14E078CE9C2895364F6C4C3F'
    $hashes = @{
        tailscale = '93C3558F592200133B377DD9F96EAC9B278B9057F7AE6B8656B15F4FA05506D0'
        tailscaled = '1FF5174FCBF3ABBFF85EACC73E6E548B0D094D913A48C1369F60811D1E92EEEF'
    }
    $ready = $true
    foreach ($name in $hashes.Keys) {
        $path = Join-Path $penBuild $name
        if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$name]) { $ready = $false }
    }
    if ($ready) { return }
    New-Item -ItemType Directory -Force $cache, $penBuild | Out-Null
    if (!(Test-Path -LiteralPath $archive)) {
        curl.exe -fL "https://pkgs.tailscale.com/stable/tailscale_${version}_arm64.tgz" -o $archive
        if ($LASTEXITCODE -ne 0) { throw 'Tailscale download failed' }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'Tailscale archive checksum does not match the pinned release' }
    $extract = Temporary-Path 'tailscale'
    try {
        New-Item -ItemType Directory -Force $extract | Out-Null
        tar --force-local -xf $archive -C $extract
        if ($LASTEXITCODE -ne 0) { throw 'Tailscale extraction failed' }
        foreach ($name in $hashes.Keys) {
            $source = Join-Path $extract "tailscale_${version}_arm64\$name"
            if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $hashes[$name]) { throw 'Tailscale binary checksum does not match the pinned release' }
            Copy-Item -LiteralPath $source -Destination (Join-Path $penBuild $name) -Force
        }
    } finally {
        Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction Ignore
    }
}

function Build-MiniAppCode {
    $quickJS = Join-Path $state 'quickjs-2020-07-05'
    $archive = Join-Path $state 'quickjs-2020-07-05.tar.xz'
    $compiler = Join-Path $state 'qjscompile'
    if (!(Test-Path -LiteralPath (Join-Path $quickJS 'quickjs.c'))) {
        New-Item -ItemType Directory -Force $state | Out-Null
        curl.exe -fL https://bellard.org/quickjs/quickjs-2020-07-05.tar.xz -o $archive
        if ($LASTEXITCODE -ne 0) { throw 'QuickJS download failed' }
        & wsl.exe --exec bash --noprofile --norc -c "tar -xf '$(ConvertTo-WSLPath $archive)' -C '$(ConvertTo-WSLPath $state)'"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    $source = Join-Path $miniAppSource 'qjscompile.c'
    $config = Join-Path $miniAppSource 'qjsconfig.h'
    if (!(Test-Path -LiteralPath $compiler) -or (Get-Item -LiteralPath $compiler).LastWriteTime -lt (Get-Item -LiteralPath $source).LastWriteTime -or (Get-Item -LiteralPath $compiler).LastWriteTime -lt (Get-Item -LiteralPath $config).LastWriteTime) {
        $miniAppSourceWSL = ConvertTo-WSLPath $miniAppSource
        $quickJSWSL = ConvertTo-WSLPath $quickJS
        $compilerWSL = ConvertTo-WSLPath $compiler
        & wsl.exe --exec bash --noprofile --norc -c "cc -O2 -Wno-discarded-qualifiers -I '$quickJSWSL' -include '$miniAppSourceWSL/qjsconfig.h' -o '$compilerWSL' '$miniAppSourceWSL/qjscompile.c' '$quickJSWSL/quickjs.c' '$quickJSWSL/cutils.c' '$quickJSWSL/libregexp.c' '$quickJSWSL/libunicode.c' '$quickJSWSL/libbf.c' -lm -ldl -lpthread"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
        $stage = Temporary-Path 'qjs'
    try {
        New-Item -ItemType Directory -Force $stage | Out-Null
        Copy-Item (Join-Path $miniAppSource 'app.js') -Destination $stage
        $index = Get-Content (Join-Path $miniAppSource 'index.js') -Raw -Encoding UTF8
        $appID = [string](Get-MiniAppManifest).appid
        if (!$index.Contains('__APPID__')) { throw 'The miniapp source is invalid' }
        [IO.File]::WriteAllText((Join-Path $stage 'index.js'), $index.Replace('__APPID__', $appID), [Text.UTF8Encoding]::new($false))
        $stageWSL = ConvertTo-WSLPath $stage
        $compilerWSL = ConvertTo-WSLPath $compiler
    New-Item -ItemType Directory -Force $penBuild | Out-Null
    $penBuildWSL = ConvertTo-WSLPath $penBuild
    & wsl.exe --exec bash --noprofile --norc -c "'$compilerWSL' '$stageWSL/app.js' '$penBuildWSL/app.js.bin' && '$compilerWSL' '$stageWSL/index.js' '$penBuildWSL/index.js.bin' module"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction Ignore
    }
}

function Build {
    New-Item -ItemType Directory -Force $target, $penBuild, $out | Out-Null
    & cargo build --release --target-dir $target --manifest-path (Join-Path $root 'Cargo.toml')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Build-Daemon
    Fetch-Tailscale
    Build-MiniAppCode
    Build-Package $package
    & (Join-Path $target 'release\pendesk.exe') stop
    if ($LASTEXITCODE -ne 0) { throw 'Could not stop the running host' }
    Start-Sleep -Milliseconds 200
    Copy-Item (Join-Path $target 'release\pendesk.exe') -Destination $hostExecutable -Force
}

function New-AMR([string]$Source, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stream = [IO.File]::Open($Destination, [IO.FileMode]::Create)
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
        try {
            Get-ChildItem -LiteralPath $Source -Directory -Recurse | Sort-Object FullName | ForEach-Object { $archive.CreateEntry($_.FullName.Substring($Source.Length + 1).Replace('\', '/') + '/') | Out-Null }
            Get-ChildItem -LiteralPath $Source -File -Recurse | Sort-Object FullName | ForEach-Object {
                $entry = $archive.CreateEntry($_.FullName.Substring($Source.Length + 1).Replace('\', '/'), [IO.Compression.CompressionLevel]::Optimal)
                $input = [IO.File]::OpenRead($_.FullName)
                $output = $entry.Open()
                try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
}

function Build-Package([string]$Output) {
    $manifest = Get-MiniAppManifest
    $miniAppFiles = @(
        (Join-Path $miniAppSource 'app.js'),
        (Join-Path $miniAppSource 'app_icon.png'),
        (Join-Path $miniAppSource 'index.js'),
        (Join-Path $miniAppSource 'GoogleSansFlex.ttf')
    )
    $miniAppIcons = @(
        (Join-Path $miniAppSource 'back.png'),
        (Join-Path $miniAppSource 'file.png'),
        (Join-Path $miniAppSource 'folder.png'),
        (Join-Path $miniAppSource 'kb.png'),
        (Join-Path $miniAppSource 'sound.png')
    )
    $requiredFiles = $miniAppFiles + $miniAppIcons + @(
        (Join-Path $miniAppSource 'run'),
        (Join-Path $miniAppSource 'adb-guard'),
        (Join-Path $penBuild 'app.js.bin'),
        (Join-Path $penBuild 'index.js.bin'),
        (Join-Path $penBuild 'daemon'),
        (Join-Path $penBuild 'tailscale'),
        (Join-Path $penBuild 'tailscaled')
    )
    $requiredFiles | ForEach-Object { Require $_ }
    $stage = Temporary-Path 'amr'
    try {
        New-Item -ItemType Directory -Force (Join-Path $stage 'bin') | Out-Null
        Copy-Item $miniAppFiles -Destination $stage
        Copy-Item $miniAppIcons -Destination $stage
        Copy-Item (Join-Path $miniAppSource 'run') -Destination (Join-Path $stage 'bin\run')
        Copy-Item (Join-Path $miniAppSource 'adb-guard') -Destination (Join-Path $stage 'bin\adb-guard')
        Copy-Item (Join-Path $penBuild 'app.js.bin') -Destination $stage
        Copy-Item (Join-Path $penBuild 'index.js.bin') -Destination $stage
        Copy-Item (Join-Path $penBuild 'daemon') -Destination (Join-Path $stage 'bin\daemon')
        Copy-Item (Join-Path $penBuild 'tailscale') -Destination (Join-Path $stage 'bin\tailscale')
        Copy-Item (Join-Path $penBuild 'tailscaled') -Destination (Join-Path $stage 'bin\tailscaled')
        $index = Get-Content (Join-Path $stage 'index.js') -Raw -Encoding UTF8
        [IO.File]::WriteAllText((Join-Path $stage 'index.js'), $index.Replace('__APPID__', [string]$manifest.appid), [Text.UTF8Encoding]::new($false))
        $cert = [ordered]@{}
        Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
            $name = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            $cert[$name] = [ordered]@{ size = $_.Length; md5 = (Get-FileHash $_.FullName -Algorithm MD5).Hash.ToLower() }
        }
        $manifest | Add-Member -NotePropertyName cert -NotePropertyValue $cert
        [IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
        New-AMR $stage $Output
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction Ignore
    }
}

function Configure {
    Confirm-Pen
    $app = Installed-App
    $tailscale = TailscalePath
    $localAddress = (& $tailscale ip -4 | ForEach-Object { $_.Trim() } | Select-Object -First 1)
    if (!$hostaddress) { $hostaddress = $localAddress }
    $localHost = $hostaddress -eq $localAddress
    $address = $null
    if (![Net.IPAddress]::TryParse($hostaddress, [ref]$address) -or $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or $address.GetAddressBytes()[0] -ne 100 -or $address.GetAddressBytes()[1] -lt 64 -or $address.GetAddressBytes()[1] -gt 127) { throw 'HostAddress must be a Tailnet IPv4 address' }
    if (!$localHost -and !$token) { throw 'Provide the remote host pairing token with -Token when using another HostAddress' }
    if ($localHost) {
        Require $hostExecutable
        $status = (& $tailscale status --json) | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or $status.BackendState -ne 'Running') { throw 'Sign in to Windows Tailscale before pairing the pen' }
    }
    $config = Join-Path $state 'active-config'
    $previous = if ($localHost -and (Test-Path -LiteralPath $config)) { ConvertFrom-StringData (Get-Content -LiteralPath $config -Raw) } else { @{} }
    if (!$token -and $previous.token) { $token = $previous.token }
    if (!$portSpecified -and $previous.port) { $port = [int]$previous.port }
    if (!$token) {
        $bytes = [byte[]]::new(32)
        $random = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $random.GetBytes($bytes) } finally { $random.Dispose() }
        $token = ([BitConverter]::ToString($bytes) -replace '-', '').ToLower()
    }
    if ($token -notmatch '^[0-9a-fA-F]{64}$') { throw 'Token must contain 64 hexadecimal characters' }
    $private = Temporary-Path 'pairing'
    New-Item -ItemType Directory -Path $private | Out-Null
    Protect-Path $private
    $candidateConfig = Join-Path $private 'config'
    try {
        [IO.File]::WriteAllText($candidateConfig, "host=$hostaddress`nport=$port`nsocks=127.0.0.1:1055`ntoken=$token`n", [Text.UTF8Encoding]::new($false))
        Invoke-Pen "mkdir -p '$($app.data)'; chmod 700 '$($app.data)'" | Out-Null
        Invoke-ADB @('-s', $serial, 'push', $candidateConfig, "$($app.data)/config.new") | Out-Null
        Invoke-Pen "/bin/sh '$($app.start)' configure" | Out-Null
        if ($localHost) {
            New-Item -ItemType Directory -Force $state | Out-Null
            Copy-Item -LiteralPath $candidateConfig -Destination $config -Force
            Protect-Path $config
        }
    } finally {
        Remove-Item -LiteralPath $private -Recurse -Force -ErrorAction Ignore
    }
    Write-Output 'Mode: direct connection preferred; DERP fallback selected automatically.'
    Login-Pen
    if ($localHost) {
        & $hostExecutable '+startup'
        if ($LASTEXITCODE -ne 0) { throw 'Could not configure PenDesk startup' }
        & $hostExecutable restart
        if ($LASTEXITCODE -ne 0) { throw 'Could not start PenDesk' }
    }
    Write-Output "Host: $hostaddress"
    if ($localHost) { Write-Output "Config: $config" }
    Write-Output 'Pairing was saved as runtime data only; the AMR contains no configuration or identity.'
}

switch ($Action) {
    build { Build }
    configure { Configure }
    install { Install-Package $package }
}
