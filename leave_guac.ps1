$sessionId = (Get-Process -Id $PID).SessionId

Write-Host "Handing session $sessionId back to console..."

tscon $sessionId /dest:console