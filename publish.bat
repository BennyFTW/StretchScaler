@echo off
rem One-click publish: commits all your changes and pushes them to GitHub.
rem GitHub Actions then builds the exe automatically. Optionally tags a version, which creates a
rem downloadable Release (the "Download" link in the README always points to the newest one).
setlocal EnableExtensions
cd /d "%~dp0"

git rev-parse --is-inside-work-tree >nul 2>&1 || (echo [ERROR] This folder is not a git repository. & goto end)

git add -A
git diff --cached --quiet
if not errorlevel 1 (
    echo No changes to publish.
    goto release
)

echo Changes to publish:
git status --short
echo.
set "MSG="
set /p "MSG=Describe your change (one line): "
if not defined MSG set "MSG=Update"
git commit -q -m "%MSG%" || (echo [ERROR] Commit failed. & goto end)

:push
git push -q origin HEAD || (echo [ERROR] Push failed - check your internet / GitHub login ^(gh auth status^). & goto end)
echo [OK] Pushed. GitHub is building it now (see the Actions tab).

:release
echo.
set "LAST="
for /f "delims=" %%t in ('git describe --tags --abbrev^=0 2^>nul') do set "LAST=%%t"
if defined LAST (echo Latest release: %LAST%) else (echo No releases yet.)
set "VER="
set /p "VER=New release version (e.g. 1.0.1) - leave empty to skip: "
if not defined VER goto end
git tag -a "v%VER%" -m "StretchScaler v%VER%" || (echo [ERROR] Could not create tag v%VER%. & goto end)
git push -q origin "v%VER%" || (echo [ERROR] Could not push tag. & goto end)
echo [OK] Release v%VER% is being built and will appear under Releases in a few minutes.

:end
echo.
pause
endlocal
