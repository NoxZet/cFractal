if (-not (Test-Path -Path ".\out")) {
    New-Item -Path "." -Name "out" -ItemType "Directory"
}

gcc src\mandelbrot.c src\renderer.c src\window.c -o out\brot.exe -lgdi32 -lwinmm
if ( $LastExitCode -ne 0)
{
    echo "Failed to compile"
    Exit
}

if ((Test-Path -path .\mtLocation.cfg))
{
    $text = Get-Content .\mtLocation.cfg -Raw
    $out = cmd /c "`"$text`" -manifest `"brot.manifest`" -outputresource:`"out\brot.exe`";#1"
    if ( $LastExitCode -ne 0)
    {
        echo "Failed to modify manifest"
        echo $out
        Exit
    }
}
else
{
    echo "mtLocation.cfg not found. Configure this file with path to mt.exe file."
}

Remove-Item "out\brot.log"
echo "Starting brot.exe"
.\out\brot.exe *>"out\brot.log"
