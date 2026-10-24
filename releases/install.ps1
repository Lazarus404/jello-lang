# SPDX-License-Identifier: BSD-3-Clause
# Installs the Jello release beside this script (package.sh lays it out) for the current user, on
# the Windows machine it was built for.
#
#   <prefix>\sdk\<version>\   bin, lib, include, std and licenses
#   <prefix>\current          the version in use, a directory junction
#
# The user's environment gets <prefix>\current\bin on PATH, and JELLO_ROOT and JELLO_STD_PATH,
# where jelloc finds the standard library; the uninstaller takes them away again.
#
#   install.ps1 [-Prefix dir] [-NoPath]
#   install.ps1 -Uninstall [-Prefix dir]
#
#   -Prefix dir   install under dir (default %LOCALAPPDATA%\jello)
#   -NoPath       set JELLO_ROOT and JELLO_STD_PATH, but leave PATH alone
#   -Uninstall    remove this version, and, when no version is left in use, the environment
#                 it set
[CmdletBinding()]
param(
  [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'jello'),
  [switch]$NoPath,
  [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

function Fail([string]$message) {
  [Console]::Error.WriteLine("install.ps1: $message")
  exit 1
}

$here = $PSScriptRoot
$versionFile = Join-Path $here 'VERSION'
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
  Fail 'no VERSION beside it (run package.sh)'
}
$fields = (Get-Content -LiteralPath $versionFile -Raw).Trim() -split '\s+'
$version, $commit, $releaseOs, $arch = $fields

# The processor this Windows runs on, as the machine's environment holds it: the process's own
# says what an emulated PowerShell is emulating.
$processor = [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE', 'Machine')
$machine = switch ($processor) { 'AMD64' { 'x64' } 'ARM64' { 'arm64' } default { $processor } }

New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
$Prefix = (Resolve-Path -LiteralPath $Prefix).ProviderPath
$sdk = Join-Path $Prefix 'sdk'
$target = Join-Path $sdk $version
$current = Join-Path $Prefix 'current'
$bin = Join-Path $current 'bin'

# The user's own PATH, as the registry holds it: without the process's additions, and with its
# %VARIABLE% references kept, which [Environment] would expand and write back expanded. Setting a
# variable through [Environment] afterwards tells running programs the environment changed.
$userEnvironment = 'HKCU:\Environment'
function Get-UserPath {
  (Get-Item -LiteralPath $userEnvironment).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
}
function Set-UserPath([string]$value) {
  if ($value) {
    Set-ItemProperty -LiteralPath $userEnvironment -Name Path -Value $value -Type ExpandString
  } else {
    Remove-ItemProperty -LiteralPath $userEnvironment -Name Path -ErrorAction SilentlyContinue
  }
}
# Empty entries stay, such as a trailing `;`, so uninstalling leaves PATH exactly as it was.
function Remove-FromPath([string]$path, [string]$dir) {
  (($path -split ';') | Where-Object { $_.TrimEnd('\') -ne $dir.TrimEnd('\') }) -join ';'
}

# Where the junction `current` points, or $null when it is not one.
function Get-JunctionTarget([string]$path) {
  $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
  if ($null -eq $item -or $item.LinkType -ne 'Junction') { return $null }
  @($item.Target)[0]
}

if ($Uninstall) {
  $points = Get-JunctionTarget $current
  if ($points -and ($points.TrimEnd('\') -eq $target)) {
    # Removes the junction, not what it points at.
    [IO.Directory]::Delete($current)
  }
  if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
  foreach ($dir in @($sdk, $Prefix)) {
    if ((Test-Path -LiteralPath $dir) -and -not (Get-ChildItem -LiteralPath $dir -Force)) {
      Remove-Item -LiteralPath $dir -Force
    }
  }
  # The variables go only when they are this prefix's, not another install's.
  $root = [Environment]::GetEnvironmentVariable('JELLO_ROOT', 'User')
  if (-not (Test-Path -LiteralPath $current) -and (-not $root -or ($root -eq $current))) {
    Set-UserPath (Remove-FromPath (Get-UserPath) $bin)
    [Environment]::SetEnvironmentVariable('JELLO_ROOT', $null, 'User')
    [Environment]::SetEnvironmentVariable('JELLO_STD_PATH', $null, 'User')
  }
  Write-Output "Uninstalled Jello $version from $Prefix."
  exit 0
}

foreach ($item in @('bin\jello.exe', 'bin\jelloc.exe', 'bin\jlo_tool.exe', 'lib\std.jdll',
    'lib\tls.jdll', 'lib\libjello_vm.a', 'include\jello.h', 'include\jello\jdll.h',
    'include\jello.manifest', 'std\core\Array.jello')) {
  if (-not (Test-Path -LiteralPath (Join-Path $here $item) -PathType Leaf)) {
    Fail "the release has no $item (run package.sh)"
  }
}
if ("$releaseOs $arch" -ne "windows $machine") {
  Fail "this release is for $releaseOs $arch, not windows $machine"
}

Write-Output "Installing Jello $version ($commit) in $target"
New-Item -ItemType Directory -Force -Path $sdk | Out-Null
$staging = "$target.new"
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Path $staging | Out-Null
foreach ($item in @('bin', 'lib', 'include', 'std', 'licenses', 'VERSION', 'README.md')) {
  Copy-Item -LiteralPath (Join-Path $here $item) -Destination $staging -Recurse
}
# A release a browser downloaded is marked as from the internet, and Windows would warn before
# running each of its programs.
Get-ChildItem -LiteralPath $staging -Recurse -File | Unblock-File

$points = Get-JunctionTarget $current
if (Test-Path -LiteralPath $current) {
  if ($null -eq $points) { Fail "$current is in the way: it is not a junction this installer made" }
  [IO.Directory]::Delete($current)
}
if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
Move-Item -LiteralPath $staging -Destination $target
New-Item -ItemType Junction -Path $current -Target $target | Out-Null

if (-not $NoPath) {
  Set-UserPath ((@($bin, (Remove-FromPath (Get-UserPath) $bin)) | Where-Object { $_ }) -join ';')
}
[Environment]::SetEnvironmentVariable('JELLO_ROOT', $current, 'User')
[Environment]::SetEnvironmentVariable('JELLO_STD_PATH', (Join-Path $current 'std'), 'User')

# The installed tools, run as a new terminal would find them, in a scratch directory.
$check = Join-Path ([IO.Path]::GetTempPath()) ("jello-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $check | Out-Null
try {
  Set-Content -LiteralPath (Join-Path $check 'hello.jello') -Encoding ascii `
    -Value "let who = `"Jello`";`n`"hello from `" + who"
  $env:JELLO_ROOT = $current
  $env:JELLO_STD_PATH = Join-Path $current 'std'
  $jelloc = Join-Path $bin 'jelloc.exe'
  $jello = Join-Path $bin 'jello.exe'
  # Runs a tool, its output kept and its failure fatal.
  function Invoke-Tool([string]$tool, [string[]]$arguments) {
    $output = & $tool @arguments
    if ($LASTEXITCODE -ne 0) { Fail 'the installed tools do not run' }
    $output
  }
  Push-Location -LiteralPath $check
  try {
    Invoke-Tool $jelloc @('hello.jello', '--out', 'hello.jlo')
    $first = Invoke-Tool $jello @('hello.jlo')
    $second = Invoke-Tool $jelloc @('run', 'hello.jello')
  } finally {
    Pop-Location
  }
  $result = (@($first) + @($second)) -join "`n"
  if ($result -ne "hello from Jello`nhello from Jello") {
    Fail "the installed tools printed: $result"
  }
} finally {
  Remove-Item -LiteralPath $check -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output "Installed Jello ${version}: jello, jelloc and jlo_tool in $bin."
if ($NoPath) {
  Write-Output "Add $bin to PATH to use them."
} else {
  Write-Output 'Open a new terminal to use them.'
}
