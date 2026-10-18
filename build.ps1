# KTile 构建脚本 (VS2017 / C++17)
#
# 用法：
#   .\build.ps1                  # 编译主程序（Release）
#   .\build.ps1 -Debug           # Debug 构建
#   .\build.ps1 -Tests           # 编译并运行全部测试
#   .\build.ps1 -Tests -Test tile  # 只跑名称含 "tile" 的测试
#   .\build.ps1 -Clean           # 清空 build 目录
#
# 设计说明：
#   - 不使用 CMake，直接调用 cl.exe（VS2017 未装 CMake 也能跑）
#   - 静态链接 /MT，保证单文件绿色产物
#   - 目标 Win7 (0x0601)，不使用任何 Win8+ API
#   - 文件必须保存为 UTF-8 with BOM，否则 PowerShell 5.1 会误读中文

param(
    [switch]$Debug,
    [switch]$Tests,
    [string]$Test = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$Root       = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir     = Join-Path $Root "src"
$TestsDir   = Join-Path $Root "tests"
$BuildDir   = Join-Path $Root "build"
$ObjDir     = Join-Path $BuildDir "obj"
$ThirdParty = Join-Path $Root "third_party"

# ---------------------------------------------------------------- 工具链
$VcTools = "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Tools\MSVC\14.16.27023"
$SdkBin  = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64"
$SdkInc  = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.19041.0"
$SdkLib  = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0"

$Cl = Join-Path $VcTools "bin\Hostx64\x64\cl.exe"
if (-not (Test-Path $Cl)) {
    Write-Error "找不到 cl.exe: $Cl`n请确认 VS2017 安装路径是否正确。"
}

$env:Path = "$(Split-Path $Cl);$VcTools\bin\Hostx64;$SdkBin;$env:Path"
$env:INCLUDE = "$VcTools\include;$SdkInc\ucrt;$SdkInc\shared;$SdkInc\um;$SdkInc\winrt;$SdkInc\cppwinrt"
$env:LIB = "$VcTools\lib\x64;$SdkLib\ucrt\x64;$SdkLib\um\x64"

# ---------------------------------------------------------------- 目录准备
if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

# ---------------------------------------------------------------- 编译选项
if ($Debug) {
    $Config      = "Debug"
    $RuntimeFlag = "/MTd"
    $OptFlags    = @("/Od", "/Zi", "/RTC1")
} else {
    $Config      = "Release"
    $RuntimeFlag = "/MT"
    $OptFlags    = @("/O2", "/DNDEBUG")
}

$CommonFlags = @(
    "/nologo"
    "/std:c++17"
    "/EHsc"
    "/W4"
    "/permissive-"
    "/utf-8"
    $RuntimeFlag
    "/DUNICODE"
    "/D_UNICODE"
    "/DWIN32_LEAN_AND_MEAN"
    "/DNOMINMAX"
    "/D_WIN32_WINNT=0x0601"
    "/DWINVER=0x0601"
    "/I$SrcDir"
    "/I$ThirdParty"
)

$SystemLibs = @(
    "user32.lib", "gdi32.lib", "shell32.lib", "ole32.lib", "oleaut32.lib",
    "advapi32.lib", "comctl32.lib", "comdlg32.lib", "windowscodecs.lib",
    "d2d1.lib", "dwrite.lib", "shlwapi.lib", "uuid.lib"
)

# 主程序链接时带 GUI 子系统（无控制台窗口）
$GuiSubsystem = $true

# ---------------------------------------------------------------- 编译函数
function Invoke-Compile {
    param(
        [string]   $Source,
        [string]   $OutputExe,
        [string[]] $ExtraSources = @(),
        [string[]] $ExtraLibs    = @(),
        [switch]   $Gui
    )

    $sources = @($Source) + $ExtraSources
    $name    = [System.IO.Path]::GetFileName($OutputExe)
    Write-Host "  编译 $name ..." -NoNewline

    # 用响应文件（.rsp）传参，彻底绕开 PowerShell -> cmd -> cl 的三层引号转义地狱。
    # cl 的响应文件按行拆分，路径含空格也没问题（无需引号）。
    $rspLines = @()
    $rspLines += $CommonFlags
    $rspLines += $OptFlags
    $rspLines += $sources
    $rspLines += "/Fo" + $ObjDir + "\"
    $rspLines += "/Fe" + $OutputExe

    # /link 之后的所有内容必须写在「同一行」——否则 cl 会把它们当成编译器选项
    # （实测报 D9002 忽略未知选项 /SUBSYSTEM:WINDOWS）。
    $linkParts = @("/link")
    if ($Gui) {
        $linkParts += "/SUBSYSTEM:WINDOWS"
        $linkParts += "/ENTRY:wWinMainCRTStartup"
    } else {
        $linkParts += "/SUBSYSTEM:CONSOLE"
    }
    $linkParts += "/INCREMENTAL:NO"
    $linkParts += $SystemLibs
    $linkParts += $ExtraLibs
    $rspLines += ($linkParts -join " ")

    $rspPath = Join-Path $BuildDir "cl.rsp"
    # cl 的响应文件用 ANSI/UTF-8 均可，但为稳妥统一写 UTF-8 无 BOM
    [System.IO.File]::WriteAllLines(
        $rspPath, $rspLines, (New-Object System.Text.UTF8Encoding($false)))

    $compileOutput = & $Cl "@$rspPath" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host " 失败" -ForegroundColor Red
        $compileOutput |
            Where-Object { $_ -match "error|错误" } |
            Select-Object -First 25 |
            ForEach-Object { Write-Host "    $_" -ForegroundColor DarkRed }
        return $false
    }
    Write-Host " OK" -ForegroundColor Green
    return $true
}

# 收集通用源文件（每个测试目标都要链接的非 main 源文件）
function Get-CoreSources {
    $candidates = @(
        (Join-Path $SrcDir "core\tile_layout.cpp")
        (Join-Path $SrcDir "core\item_operations.cpp")
        (Join-Path $SrcDir "core\tab_operations.cpp")
        (Join-Path $SrcDir "core\search.cpp")
        (Join-Path $SrcDir "base\text_convert.cpp")
    )
    return @($candidates | Where-Object { Test-Path $_ })
}

# 持久化相关源文件（持久化测试需要）
function Get-PersistenceSources {
    $candidates = @(
        (Join-Path $SrcDir "base\text_convert.cpp")
        (Join-Path $SrcDir "infrastructure\json\json_writer.cpp")
        (Join-Path $SrcDir "infrastructure\json\json_parser.cpp")
        (Join-Path $SrcDir "infrastructure\json\config_store.cpp")
        (Join-Path $SrcDir "infrastructure\filesystem\portable_paths.cpp")
    )
    return @($candidates | Where-Object { Test-Path $_ })
}

# 收集主程序全部实现文件（平台层 + UI 层 + 应用层，不含 main.cpp）
function Get-AppSources {
    $candidates = @(
        # base
        (Join-Path $SrcDir "base\text_convert.cpp")
        # core
        (Join-Path $SrcDir "core\tile_layout.cpp")
        (Join-Path $SrcDir "core\item_operations.cpp")
        (Join-Path $SrcDir "core\tab_operations.cpp")
        (Join-Path $SrcDir "core\search.cpp")
        # infrastructure
        (Join-Path $SrcDir "infrastructure\filesystem\portable_paths.cpp")
        (Join-Path $SrcDir "infrastructure\json\json_writer.cpp")
        (Join-Path $SrcDir "infrastructure\json\json_parser.cpp")
        (Join-Path $SrcDir "infrastructure\json\config_store.cpp")
        # platform/win32
        (Join-Path $SrcDir "platform\win32\single_instance.cpp")
        (Join-Path $SrcDir "platform\win32\global_hotkey.cpp")
        (Join-Path $SrcDir "platform\win32\tray_icon.cpp")
        (Join-Path $SrcDir "platform\win32\startup_registration.cpp")
        (Join-Path $SrcDir "platform\win32\shell_link.cpp")
        (Join-Path $SrcDir "platform\win32\icon_loader.cpp")
        (Join-Path $SrcDir "platform\win32\outside_click_monitor.cpp")
        (Join-Path $SrcDir "platform\win32\popup_menu.cpp")
        (Join-Path $SrcDir "platform\win32\item_launcher.cpp")
        (Join-Path $SrcDir "platform\win32\icon_store.cpp")
        (Join-Path $SrcDir "platform\win32\drop_target.cpp")
        (Join-Path $SrcDir "platform\win32\system_actions.cpp")
        (Join-Path $SrcDir "platform\win32\builtin_icon.cpp")
        (Join-Path $SrcDir "platform\win32\svg_path.cpp")
        (Join-Path $SrcDir "platform\win32\uuid.cpp")
        # ui
        (Join-Path $SrcDir "ui\theme.cpp")
        (Join-Path $SrcDir "ui\launcher_window.cpp")
        (Join-Path $SrcDir "ui\launcher_window_behavior.cpp")
        (Join-Path $SrcDir "ui\launcher_window_animation.cpp")
        (Join-Path $SrcDir "ui\tile_context_menu.cpp")
        (Join-Path $SrcDir "ui\item_editor_dialog.cpp")
        (Join-Path $SrcDir "ui\builtin_item_dialog.cpp")
        (Join-Path $SrcDir "ui\text_input_dialog.cpp")
        (Join-Path $SrcDir "ui\about_dialog.cpp")
        (Join-Path $SrcDir "ui\settings_window.cpp")
        # app
        (Join-Path $SrcDir "app\application.cpp")
    )
    return @($candidates | Where-Object { Test-Path $_ })
}

# ---------------------------------------------------------------- 测试模式
if ($Tests) {
    Write-Host "===== KTile 测试构建 ($Config) =====" -ForegroundColor Cyan

    $testFiles = Get-ChildItem -Path $TestsDir -Filter "test_*.cpp" -ErrorAction SilentlyContinue
    if ($Test -ne "") {
        $testFiles = $testFiles | Where-Object { $_.BaseName -match $Test }
    }
    if (-not $testFiles) {
        Write-Host "未找到匹配的测试文件。" -ForegroundColor Yellow
        exit 1
    }

    $failed  = 0
    $passed  = 0
    $coreSources = Get-CoreSources

    foreach ($tf in $testFiles) {
        $exePath = Join-Path $BuildDir "$($tf.BaseName).exe"

        # 依据测试文件包含的头，决定需要附带哪些源文件
        $needsCore = $true
        $content = Get-Content $tf.FullName -Raw
        if ($content -notmatch "tile_layout\.h|item_operations\.h|tab_operations\.h|search\.h") {
            $needsCore = $false
        }
        $extra = if ($needsCore) { $coreSources } else { @() }

        if ($content -match "config_store\.h|json_writer\.h|json_parser\.h|portable_paths\.h") {
            $extra = @($extra) + (Get-PersistenceSources)
            # 避免 text_convert.cpp 重复出现导致重复符号
            $extra = $extra | Select-Object -Unique
        }

        # 图标仓库测试（WIC 解码/裁剪/编码落盘）需要带上 icon_store.cpp
        if ($content -match "icon_store\.h") {
            $extra = @($extra) + (Join-Path $SrcDir "platform\win32\icon_store.cpp")
            $extra = $extra | Select-Object -Unique
        }

        # 拖放解析测试需要带上 drop_target.cpp
        if ($content -match "drop_target\.h") {
            $extra = @($extra) + (Join-Path $SrcDir "platform\win32\drop_target.cpp")
            $extra = $extra | Select-Object -Unique
        }

        if (-not (Invoke-Compile -Source $tf.FullName -OutputExe $exePath -ExtraSources $extra)) {
            $failed++
            continue
        }

        Write-Host "  运行 $($tf.BaseName) ..." -NoNewline
        $runOutput = & $exePath 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host " PASS" -ForegroundColor Green
            $passed++
        } else {
            Write-Host " FAIL" -ForegroundColor Red
            $runOutput | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkYellow }
            $failed++
        }
    }

    Write-Host ""
    if ($failed -eq 0) {
        Write-Host "===== 全部通过 ($passed 个测试) =====" -ForegroundColor Green
        exit 0
    }
    Write-Host "===== 失败 $failed 个 / 通过 $passed 个 =====" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------- 主程序构建
Write-Host "===== KTile 构建 ($Config) =====" -ForegroundColor Cyan

$mainApp = Join-Path $SrcDir "app\main.cpp"
if (-not (Test-Path $mainApp)) {
    Write-Host "主程序入口尚未创建: $mainApp" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "当前可运行:  .\build.ps1 -Tests" -ForegroundColor Cyan
    exit 0
}

$exeOut = Join-Path $BuildDir "KTile.exe"
$appSources = Get-AppSources

# 应用图标资源：app.rc → app.res，cl 会把 .res 直接交给链接器。
# 没有这步 exe 就是「无图标」状态（任务栏/资源管理器显示通用图标）。
$rcFile = Join-Path $SrcDir "app\app.rc"
$rcExe = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\rc.exe"
if ((Test-Path $rcFile) -and (Test-Path $rcExe)) {
    $resOut = Join-Path $BuildDir "app.res"
    & $rcExe /nologo /fo $resOut $rcFile | Out-Null
    if ($LASTEXITCODE -eq 0 -and (Test-Path $resOut)) {
        $appSources = @($appSources) + $resOut
        Write-Host "  图标资源 app.res 已并入" -ForegroundColor DarkGray
    } else {
        Write-Host "  [警告] 图标资源编译失败，exe 将没有内嵌图标" -ForegroundColor Yellow
    }
}

Write-Host "  源文件 $($appSources.Count + 1) 个" -ForegroundColor DarkGray

if (Invoke-Compile -Source $mainApp -OutputExe $exeOut -ExtraSources $appSources -Gui:$GuiSubsystem) {
    $item = Get-Item $exeOut
    $sizeKb = [math]::Round($item.Length / 1KB, 1)
    Write-Host ""
    Write-Host "产物: $exeOut" -ForegroundColor Green
    Write-Host "大小: $sizeKb KB" -ForegroundColor Green
}
