@echo off
rem Starts the browser build on this PC: http://127.0.0.1:8077/  (see README, "Run it in the browser").
cd /d "%~dp0"
if not exist out\web\nfs5.wasm (echo out\web\nfs5.wasm is missing: run ./gen.sh and ./web/build.sh first & pause & exit /b 1)
if not exist web\node_modules (pushd web & call npm install & popd)
pushd web & call node build.mjs & popd
start "" "http://127.0.0.1:8077/"
node web\server.mjs %*
