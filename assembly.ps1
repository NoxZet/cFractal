if (-not (Test-Path -Path ".\out")) {
    New-Item -Path "." -Name "out" -ItemType "Directory"
}
if (-not (Test-Path -Path ".\out\asm")) {
    New-Item -Path "." -Name "out\asm" -ItemType "Directory"
}

Get-ChildItem "./src" -Filter "*.c" | ForEach-Object {
    $asmOutput = ".\out\asm\$($_.Name.Substring(0, $_.Name.Length - 2)).asm"
    gcc "$($_.FullName)" -c -S -o $asmOutput -lgdi32 -lwinmm
}