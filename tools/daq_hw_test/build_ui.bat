@echo off
setlocal
cd /d "%~dp0\..\.."
python -m pip install -r tools\daq_hw_test\requirements.txt
python -m pip install pyinstaller
python -m PyInstaller --onefile --windowed --name "DAQ半实物测试工具" tools\daq_hw_test\daq_hw_test_ui.py
endlocal
