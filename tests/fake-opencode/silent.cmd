@echo off
setlocal
where python3 >nul 2>nul
if errorlevel 1 goto :try_python
python3 -c "import time; time.sleep(30)"
exit /b %errorlevel%
:try_python
where python >nul 2>nul
if errorlevel 1 goto :try_py
python -c "import time; time.sleep(30)"
exit /b %errorlevel%
:try_py
py -3 -c "import time; time.sleep(30)"
