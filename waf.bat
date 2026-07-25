@setlocal
@set PYEXE=python
@where %PYEXE% 1>NUL 2>NUL
@if %ERRORLEVEL% neq 0 set PYEXE=py
@if not exist "%~dp0submodules\external\waf\waf-light" (
    @echo Error: Waf submodule is not initialized.
    @echo Please run the following command to initialize it:
    @echo     git submodule update --init --recursive
    @exit /b 1
)
@if not exist "%~dp0submodules\external\waf\waf" (
    @echo Waf binary is missing in the submodule. Building it using waf-light...
    @pushd "%~dp0submodules\external\waf"
    @%PYEXE% waf-light
    @popd
    @if not exist "%~dp0submodules\external\waf\waf" (
        @echo Error: Failed to build Waf using waf-light.
        @exit /b 1
    )
    @echo Waf binary built successfully.
)
@%PYEXE% -x "%~dp0submodules\external\waf\waf" %*
@exit /b %ERRORLEVEL%
