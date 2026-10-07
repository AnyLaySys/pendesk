param(
    [Parameter(Position = 0)][ValidateSet('clean')][string]$action
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$target = Join-Path $build 'target'; $out = Join-Path $build 'out'
$penBuild = Join-Path $out 'pen'
$penSource = Join-Path $root 'src\pen'; $miniAppSource = Join-Path $penSource 'miniapp'
$windowsExecutable = Join-Path $out 'pendesk.exe'
$package = Join-Path $out 'pendesk.amr'; $state = Join-Path $env:LOCALAPPDATA 'PenDesk'

function Temporary-Path([string]$Name) {
    $directory = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    $path = [IO.Path]::GetFullPath((Join-Path $directory "pendesk-$PID-$([guid]::NewGuid().ToString('N'))-$Name"))
    if (!$path.StartsWith($directory, [StringComparison]::OrdinalIgnoreCase)) { throw '路径错误' }
    $path
}

function Get-MiniAppManifest {
    $manifest = Get-Content -LiteralPath (Join-Path $miniAppSource 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$manifest.appid -notmatch '^\d+$') { throw 'MiniApp ID无效' }
    $manifest
}

function Require([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw '路径文件缺失' }
}

function Fetch-Tailscale {
    $version = '1.102.4'
    $cache = Join-Path $env:LOCALAPPDATA 'PenDesk\tailscale-release'
    $archive = Join-Path $cache "tailscale_${version}_arm64.tgz"
    $ready = (Test-Path -LiteralPath (Join-Path $penBuild 'tailscale')) -and (Test-Path -LiteralPath (Join-Path $penBuild 'tailscaled'))
    if ($ready) { return }
    New-Item -ItemType Directory -Force $cache, $penBuild | Out-Null
    if (!(Test-Path -LiteralPath $archive)) {
        curl.exe -fL "https://pkgs.tailscale.com/stable/tailscale_${version}_arm64.tgz" -o $archive
        if ($LASTEXITCODE -ne 0) { throw 'Tailscale下载失败' }
    }
    $extract = Temporary-Path 'tailscale'
    try {
        New-Item -ItemType Directory -Force $extract | Out-Null
        tar -xf $archive -C $extract
        if ($LASTEXITCODE -ne 0) { throw 'Tailscale解压失败' }
        foreach ($name in @('tailscale', 'tailscaled')) {
            $source = Join-Path $extract "tailscale_${version}_arm64\$name"
            Copy-Item -LiteralPath $source -Destination (Join-Path $penBuild $name) -Force
        }
    } finally {
        Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction Ignore
    }
}

function Build-LinuxArtifacts {
    $quickJS = Join-Path $state 'quickjs-2020-07-05'
    $archive = Join-Path $state 'quickjs-2020-07-05.tar.xz'
    $compiler = Join-Path $state 'qjscompile'
    if (!(Test-Path -LiteralPath (Join-Path $quickJS 'quickjs.c'))) {
        New-Item -ItemType Directory -Force $state | Out-Null
        if (!(Test-Path -LiteralPath $archive)) {
            curl.exe -fL https://bellard.org/quickjs/quickjs-2020-07-05.tar.xz -o $archive
            if ($LASTEXITCODE -ne 0) { throw 'QuickJS下载失败' }
        }
    }
    $stage = Temporary-Path 'qjs'
    try {
        New-Item -ItemType Directory -Force $stage | Out-Null
        Copy-Item (Join-Path $miniAppSource 'app.js') -Destination $stage
        $index = Get-Content (Join-Path $miniAppSource 'index.js') -Raw -Encoding UTF8
        $appID = [string](Get-MiniAppManifest).appid
        if (!$index.Contains('__APPID__')) { throw 'MiniApp源码无效' }
        [IO.File]::WriteAllText(
            (Join-Path $stage 'index.js'),
            $index.Replace('__APPID__', $appID),
            [Text.UTF8Encoding]::new($false)
        )
        $script = @'
set -eu
pen=$(wslpath -a "$1")
out=$(wslpath -a "$2")
quickjs=$(wslpath -a "$3")
archive=$(wslpath -a "$4")
state=$(wslpath -a "$5")
compiler=$(wslpath -a "$6")
miniapp=$(wslpath -a "$7")
stage=$(wslpath -a "$8")
sudo dpkg --add-architecture arm64
sudo apt install -y libc6-dev-arm64-cross libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
if [ ! -f "$quickjs/quickjs.c" ]; then
    tar -xf "$archive" -C "$state"
fi
cc -O2 -Wno-discarded-qualifiers -I "$quickjs" -include "$miniapp/qjsconfig.h" -o "$compiler" "$miniapp/qjscompile.c" "$quickjs/quickjs.c" "$quickjs/cutils.c" "$quickjs/libregexp.c" "$quickjs/libunicode.c" "$quickjs/libbf.c" -lm -ldl -lpthread
aarch64-linux-gnu-gcc -std=c17 -O2 -Wall -Wextra -Werror -pthread -static -I "$pen/common" -I "$pen/net" -I "$pen/input" -I "$pen/video" -I "$pen/camera" -I "$pen/audio" -I "$pen/files" "$pen/pdd.c" "$pen/common/cfg.c" "$pen/common/io.c" "$pen/common/pan.c" "$pen/common/record.c" "$pen/common/display.c" "$pen/net/link.c" "$pen/net/socks.c" "$pen/net/nonce.c" "$pen/input/input.c" "$pen/input/input_pan.c" "$pen/input/control.c" "$pen/input/devices.c" "$pen/input/keymap.c" "$pen/input/touch.c" "$pen/video/video.c" "$pen/video/control.c" "$pen/video/media_receive.c" "$pen/video/frames.c" "$pen/video/preview.c" "$pen/camera/device.c" "$pen/camera/stream.c" "$pen/audio/audio.c" "$pen/audio/mic.c" "$pen/files/files.c" "$pen/files/transport.c" "$pen/files/panel.c" "$pen/files/transfer.c" -o "$out/pdd"
aarch64-linux-gnu-gcc -std=c17 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-cast-function-type -fPIC -shared -Wl,-z,nodelete -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_68 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_68 -DGST_VERSION_MIN_REQUIRED=GST_VERSION_1_22 -DGST_VERSION_MAX_ALLOWED=GST_VERSION_1_22 $(PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig pkg-config --cflags gstreamer-app-1.0 | sed 's/-I/-isystem /g') -isystem "$quickjs" "$miniapp/bridge.c" "$pen/common/pan.c" -ldl -o "$out/libjsapi_vid.so"
aarch64-linux-gnu-gcc -std=c17 -O2 -Wall -Wextra -Werror -pthread -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_68 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_68 -I "$pen/common" $(PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig pkg-config --cflags gstreamer-app-1.0 | sed 's/-I/-isystem /g') "$pen/camera/capture.c" "$pen/common/io.c" -o "$out/pdc" $(PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig pkg-config --libs gstreamer-app-1.0)
"$compiler" "$stage/app.js" "$out/app.js.bin"
"$compiler" "$stage/index.js" "$out/index.js.bin" module
'@
        $script = $script.Replace("`r`n", "`n").Replace("`r", "")
        & wsl.exe --exec bash --noprofile --norc -c $script pendesk $penSource $penBuild $quickJS $archive $state $compiler $miniAppSource $stage
        if ($LASTEXITCODE -ne 0) { throw "Linux artifacts build failed" }
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction Ignore
    }
}

function Build {
    New-Item -ItemType Directory -Force $target, $penBuild, $out | Out-Null
    & cargo build --release --target-dir $target --manifest-path (Join-Path $root 'Cargo.toml')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Copy-Item -LiteralPath (Join-Path $target 'release\pendesk.exe') -Destination $windowsExecutable -Force
    Copy-Item -LiteralPath (Join-Path $target 'release\pendesk_camera.dll') -Destination (Join-Path $out 'pendesk_camera.dll') -Force
    Fetch-Tailscale
    Build-LinuxArtifacts
    Build-Package $package
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
        (Join-Path $miniAppSource 'cam.png'),
        (Join-Path $miniAppSource 'exit.png'),
        (Join-Path $miniAppSource 'file.png'),
        (Join-Path $miniAppSource 'folder.png'),
        (Join-Path $miniAppSource 'kb.png'),
        (Join-Path $miniAppSource 'mic.png'),
        (Join-Path $miniAppSource 'mouse.png'),
        (Join-Path $miniAppSource 'touch.png'),
        (Join-Path $miniAppSource 'view.png'),
        (Join-Path $miniAppSource 'sound.png'),
        (Join-Path $miniAppSource 'power.png'),
        (Join-Path $miniAppSource 'rf.png')
    )
    $requiredFiles = $miniAppFiles + $miniAppIcons + @(
        (Join-Path $miniAppSource 'run'),
        (Join-Path $miniAppSource 'adb-guard'),
        (Join-Path $penBuild 'app.js.bin'),
        (Join-Path $penBuild 'index.js.bin'),
        (Join-Path $penBuild 'pdd'),
        (Join-Path $penBuild 'pdc'),
        (Join-Path $penBuild 'libjsapi_vid.so'),
        (Join-Path $penBuild 'tailscale'),
        (Join-Path $penBuild 'tailscaled')
    )
    $requiredFiles | ForEach-Object { Require $_ }
    $stage = Temporary-Path 'amr'
    try {
        New-Item -ItemType Directory -Force (Join-Path $stage 'bin') | Out-Null
        Copy-Item $miniAppFiles -Destination $stage
        Copy-Item $miniAppIcons -Destination $stage
        [IO.File]::WriteAllText((Join-Path $stage 'bin\run'), (Get-Content (Join-Path $miniAppSource 'run') -Raw -Encoding UTF8).Replace("`r`n", "`n").Replace("`r", ""), [Text.UTF8Encoding]::new($false))
        Copy-Item (Join-Path $miniAppSource 'adb-guard') -Destination (Join-Path $stage 'bin\adb-guard')
        Copy-Item (Join-Path $penBuild 'app.js.bin') -Destination $stage
        Copy-Item (Join-Path $penBuild 'index.js.bin') -Destination $stage
        New-Item -ItemType Directory -Force (Join-Path $stage 'libs') | Out-Null
        Copy-Item (Join-Path $penBuild 'libjsapi_vid.so') -Destination (Join-Path $stage 'libs/libjsapi_vid.so')
        Copy-Item (Join-Path $penBuild 'pdd') -Destination (Join-Path $stage 'bin\pdd')
        Copy-Item (Join-Path $penBuild 'pdc') -Destination (Join-Path $stage 'bin\pdc')
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

function Clean {
    Remove-Item -LiteralPath $build -Recurse -Force -ErrorAction Ignore
}

switch ($action) {
    clean { Clean }
    default { Build }
}
