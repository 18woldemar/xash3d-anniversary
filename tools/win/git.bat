@echo off
rem Stands in for git on the build VM, which has none: waf asks for the commit, the branch and the commit date
rem (gitversion.py, public/wscript), and the answers are in .gitinfo, which tools/package writes into each tree.
setlocal
for /f "usebackq tokens=1,* delims==" %%a in (".gitinfo") do set "%%a=%%b"
if "%1"=="describe" echo %GIT_DESCRIBE%
if "%1"=="rev-parse" echo %GIT_BRANCH%
if "%1"=="log" echo %GIT_DATE%
