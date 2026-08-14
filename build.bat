@echo off
setlocal
set "GCC=C:\Users\11407\scoop\apps\gcc\current\bin\g++.exe"
if not exist "%GCC%" (echo [!!] g++ not found: %GCC% & exit /b 1)
echo === build MCInGame.dll ===
"%GCC%" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -Iinclude -Iinclude\win32 -o MCInGame.dll src\MCInGame.cpp -lgdi32 -luser32
if errorlevel 1 goto :err
echo === build injector.exe ===
"%GCC%" -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -o injector.exe src\injector.cpp -luser32
if errorlevel 1 goto :err
echo === build test\swapclient\native\swapstub.dll (hook 冒烟测试辅助库) ===
"%GCC%" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -Iinclude -Iinclude\win32 -o test\swapclient\native\swapstub.dll test\swapclient\native\swapstub.cpp -lgdi32
if errorlevel 1 goto :err
echo.
echo [OK] done: MCInGame.dll + injector.exe
exit /b 0
:err
echo [!!] build failed
exit /b 1
