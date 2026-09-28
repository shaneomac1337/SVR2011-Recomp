# Compile the portable launcher with the .NET Framework compiler that ships with Windows.
. "$PSScriptRoot/Invoke-Hidden.ps1"
function Build-PortableLauncher([string]$OutputPath, [string]$IconPath) {
    $framework = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
    # This compiler misreads forward-slash paths, so pass only full Windows paths.
    $source = [IO.Path]::GetFullPath("$PSScriptRoot/../launcher/portable")
    $output = [IO.Path]::GetFullPath($OutputPath)
    $arguments = @('/nologo', '/target:winexe', '/platform:x64', '/optimize+', "/out:$output",
        "/lib:$framework\WPF", '/r:PresentationFramework.dll', '/r:PresentationCore.dll', '/r:WindowsBase.dll',
        '/r:System.Xaml.dll', '/r:System.Web.Extensions.dll', '/r:System.Core.dll',
        "/resource:$source\Portable.xaml,Svr2011Launcher.Portable.xaml",
        "$source\Launcher.cs", "$source\Settings.cs", "$source\DiscImage.cs", "$source\KeyArt.cs")
    if ($IconPath) { $arguments += "/win32icon:$([IO.Path]::GetFullPath($IconPath))" }
    Invoke-Hidden "$framework\csc.exe" $arguments -LogPath "$output.build.log"
    Remove-Item -LiteralPath "$output.build.log", "$output.build.log.stderr"
}
