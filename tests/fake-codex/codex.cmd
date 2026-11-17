@echo off
setlocal
where python3 >nul 2>nul
if errorlevel 1 goto :try_python
python3 "%~dp0codex" %*
exit /b %errorlevel%
:try_python
where python >nul 2>nul
if errorlevel 1 goto :try_py
python "%~dp0codex" %*
exit /b %errorlevel%
:try_py
py -3 "%~dp0codex" %*
