@echo off
setlocal
cd /d "%~dp0pogls_engine"
start "" http://127.0.0.1:8766/chat
python -m uvicorn browser_chat_server:app --host 127.0.0.1 --port 8766
