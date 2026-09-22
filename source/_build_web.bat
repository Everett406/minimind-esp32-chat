@echo off
set PYTHONIOENCODING=utf-8
cd /d D:\minimind-esp32\YuanDiArduino
D:\arduino-cli\arduino-cli.exe compile --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=none" --build-property "build.custom_partitions=partitions.csv" --build-property "compiler.cpp.extra_flags=-DCONFIG_ESP_TASK_WDT=n" --build-property "compiler.c.extra_flags=-DCONFIG_ESP_TASK_WDT=n" --build-path "D:\minimind-esp32\YuanDiArduino\.build-web" D:\minimind-esp32\YuanDiArduino 1> "D:\minimind-esp32\YuanDiArduino\build-web-stdout.log" 2> "D:\minimind-esp32\YuanDiArduino\build-web-stderr.log"
echo EXITCODE=%ERRORLEVEL% >> "D:\minimind-esp32\YuanDiArduino\build-web-stderr.log"
