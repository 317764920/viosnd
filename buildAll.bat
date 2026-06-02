@echo off
if "%VIRTIO_WIN_NO_ARM%"=="" call ..\build\build.bat viosnd.sln "Win10 Win11" ARM64
if errorlevel 1 goto :eof
if "%~1"=="" (
  call ..\build\build.bat viosnd.sln "Win10 Win11" amd64
) else (
  call ..\build\build.bat viosnd.sln "Win10 Win11" %*
)
