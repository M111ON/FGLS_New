@echo off
setlocal
cd /d "%~dp0"
python llm_memory_ingest.py --gui
