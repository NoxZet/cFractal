if (-not (Test-Path -Path ".\out")) {
    New-Item -Path "." -Name "out" -ItemType "Directory"
}

gcc src/mandelbrot.c src/renderer.c src/window.c -o out\brot.exe -lgdi32 -lwinmm -gdwarf-2
if ( $LastExitCode -ne 0)
{
    echo "Failed to compile"
    Exit
}

if ((Test-Path -path .\drmemLocation.cfg))
{
    $text = Get-Content .\drmemLocation.cfg -Raw

    Remove-Item "out\brot.log"
    echo "Starting drmemory brot.exe"
    cmd /c "`"$text`" `"out\brot.exe`"" *>"out\brot.log"
}
else
{
    echo "drmemLocation.cfg not found. Configure this file with path to mt.exe file."
}
