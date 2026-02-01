# cFractal

This is a real-time fractal viewer written in pure C. The user is able to pan and zoom around the fractal in real time. Eventually, the palette should be easily configurable.

Currently the project is only targetting x86-64 Windows 10+. I might or might not make it for Linux later.

Mainly, it is an exercise for me in C, multithreading, and optimizing calculations.

To do:
- Worker threads to split rendering work into more threads.
- Higher (arbitrary?) precision math.

To run, you need to have gcc installed (MinGW is supported).
- `run.ps1` compiles and executes `brot.exe`
- `runDrMem.ps1` compiles the program with `-gdwarf-2` argument and executes `drmemory brot.exe`. You must include drmemLocation.cfg file with the path to drmemory executable as its only contents.
- `assembly.ps1` compiles each c file into an assembly file without producing an executable.

It is recommended to create a mtLocation.cfg file with a path to Windows SDK mt.exe file as its only contents. This ensures Windows does not scale the rendered image by setting the executable's manifest.
