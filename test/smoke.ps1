# smoke.ps1 —— 冒烟测试: 假客户端 (TestSwapClient + swapstub) 中验证
#   1. 注入 + PEB 摘链 (模块枚举不可见)
#   2. gdi32!SwapBuffers 钩子 + JNI 解析 (map=mojang1201)
#   3. 状态采样 (canAtk/canPlace 随 5 个阶段变化)
#   4. 游戏内连点 (debugLogClicks=1 时记录每次 click, 平均 ~20/s)
#
# 用法: pwsh -File test\smoke.ps1 [-Java <java.exe 路径>]   (需先 build.bat)
param(
    [string]$Java = 'C:\Program Files\Microsoft\jdk-11.0.16.101-hotspot\bin\java.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if (-not (Test-Path $Java)) {
    Write-Host "[!!] 找不到 java: $Java"; exit 1
}

$log = Join-Path $env:TEMP 'MCInGameClicker.log'
$iniDir = Join-Path $env:APPDATA 'MCInGameClicker'
$ini = Join-Path $iniDir 'settings.ini'
Remove-Item $log -ErrorAction SilentlyContinue

# 调试设置: 总开关+左右键+保持连点, 记录每次点击
New-Item -ItemType Directory -Force -Path $iniDir | Out-Null
@"
master=1
left=1
right=1
cpsLeft=20
cpsRight=20
keep=1
gatk=0
gplace=0
gcursor=0
dbgClicks=1
"@ | Set-Content -Path $ini -Encoding ascii

# 启动假客户端 (后台)
$p = Start-Process -FilePath $Java `
    -ArgumentList '-cp','test\swapclient\out11','TestSwapClient' `
    -WorkingDirectory $root `
    -RedirectStandardOutput (Join-Path $root 'test\swapclient\run.log') `
    -RedirectStandardError (Join-Path $root 'test\swapclient\run.err.log') `
    -WindowStyle Hidden -PassThru
Write-Host "swapclient started pid=$($p.Id)"

Start-Sleep -Seconds 2

# 找真正的 javaw/java 进程 (Start-Process java.exe 可能就是它)
$proc = Get-CimInstance Win32_Process -Filter "Name like 'java%'" |
    Where-Object { $_.CommandLine -match 'TestSwapClient' } | Select-Object -First 1
if (-not $proc) { Write-Host '[!!] 未找到 swapclient 进程'; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; exit 1 }
Write-Host "target pid=$($proc.ProcessId)"

# 注入
& "$root\injector.exe" -pid $proc.ProcessId
Write-Host "injector exit=$LASTEXITCODE"

# 等测试跑完 (总时长约 16s)
Start-Sleep -Seconds 17

# 收尾
Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Remove-Item $ini -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '================ MCInGameClicker.log (头 8 行 + 尾 8 行) ================'
if (Test-Path $log) {
    Get-Content $log | Select-Object -First 8
    Write-Host '  ... (中间省略) ...'
    Get-Content $log | Select-Object -Last 8
} else { Write-Host '[!!] 无日志文件' }

Write-Host ''
Write-Host '================ 检查点 ================'
if (-not (Test-Path $log)) { Write-Host 'FAIL: 无日志'; exit 1 }
$c = Get-Content $log -Raw
$checks = @{
    'PEB 摘链'     = ($c -match 'peb-hidden=1');
    '钩子安装'     = ($c -match 'hook-ok');
    'JNI 就绪'     = ($c -match 'ready: map=mojang');
    '状态变化'     = ($c -match 'status canAtk=');
    '点击日志'     = ($c -match 'click L down');
}
$ok = $true
foreach ($k in $checks.Keys) {
    $v = $checks[$k]
    Write-Host ("{0}: {1}" -f $k, ($(if ($v) { 'PASS' } else { 'FAIL' })))
    if (-not $v) { $ok = $false }
}
# 点击速率粗查 (阶段2/3/4 共 10s, 20 CPS 应有 ~100+ 个 down)
$downs = ([regex]::Matches($c, 'click L down')).Count
Write-Host ("click L down 总数: {0} (预期 100~260)" -f $downs)
if ($downs -lt 60) { $ok = $false; Write-Host 'FAIL: 点击数量偏少' }
Write-Host $(if ($ok) { 'SMOKE PASS' } else { 'SMOKE FAIL' })
if (-not $ok) { exit 1 }
exit 0
