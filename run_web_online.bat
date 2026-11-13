@echo off
rem nfs5-web with the public online server (docs/tasks/ON1-online.md): the game runs from this PC as with run_web.bat,
rem its virtual network goes to wss://nfs5-reforged.duckdns.org/lan. Main menu -> Online.
cd /d "%~dp0"
if not exist out\web\nfs5.wasm (echo out\web\nfs5.wasm is missing: run ./gen.sh and ./web/build.sh first & pause & exit /b 1)
if not exist web\node_modules (pushd web & call npm install & popd)
pushd web & call node build.mjs & popd
start "" "http://127.0.0.1:8077/?lan=wss://nfs5-reforged.duckdns.org/lan"
node web\server.mjs %*
