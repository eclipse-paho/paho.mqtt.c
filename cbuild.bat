REM start broker and proxy
REM Start-Process C:\Python36\python -ArgumentList 'test\mqttsas.py'
REM Start-Process C:\Python36\python -ArgumentList 'startbroker.py -c localhost_testing.conf'

setlocal
set APPVEYOR_BUILD_FOLDER=%cd%

rmdir /s /q build.paho
mkdir build.paho

cd build.paho

REM call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" x64
call "j:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"

cmake -G "NMake Makefiles" -DPAHO_WITH_SSL=TRUE -DPAHO_BUILD_DOCUMENTATION=FALSE -DPAHO_BUILD_SAMPLES=TRUE -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=TRUE ..

nmake

ctest -T test -VV

cd ..

endlocal
