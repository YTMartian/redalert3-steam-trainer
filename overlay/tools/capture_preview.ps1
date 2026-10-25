# 抓 ra3_skin_preview 窗口截图（开发期肉眼核对配色用）。
# 用法：.\capture_preview.ps1 <themeIndex> <outPng>
param(
    [int]$Theme = 0,
    [string]$Out = "shot.png",
    [string]$Flags = ""
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinCap {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@

$exe = Join-Path $PSScriptRoot "..\bin\preview\ra3_skin_preview.exe"
$argList = @("--theme=$Theme")
if ($Flags) { $argList += $Flags.Split(" ") }
$p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru
Start-Sleep -Milliseconds 2200
$p.Refresh()

$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Error "window not found"; $p | Stop-Process -Force; exit 1 }

$r = New-Object WinCap+RECT
[void][WinCap]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left
$hh = $r.Bottom - $r.Top

$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 2 = PW_RENDERFULLCONTENT，D3D 窗口必须用这个，否则抓到黑图。
[void][WinCap]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc)
$g.Dispose()

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
$p | Stop-Process -Force
Write-Output "saved $Out ($w x $hh)"
