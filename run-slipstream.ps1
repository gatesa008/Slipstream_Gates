$env:SLIPSTREAM_ICE_PORT_BEGIN="50000"
$env:SLIPSTREAM_ICE_PORT_END="50100"
$env:SLIPSTREAM_ENABLE_ICE_TCP="0"
$env:SLIPSTREAM_ENABLE_PORT_MAPPING="0"

.\build\bin\Release\SlipStream.exe --debug *>&1 |
    Tee-Object -FilePath .\slipstream-debug.log