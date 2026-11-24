param(
    [string]$Port    = "COM6",
    [int]$BaudRate   = 9600,
    [string]$Server  = "http://localhost:3000"
)

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  ASHA RO WATER - Card Scan Bridge" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Port:   $Port" -ForegroundColor White
Write-Host "  Server: $Server" -ForegroundColor White
Write-Host "  Card scan karo - UID web form mein jayega!" -ForegroundColor Yellow
Write-Host "  Rokne ke liye: Ctrl+C" -ForegroundColor Gray
Write-Host ""

try {
    $serialPort             = New-Object System.IO.Ports.SerialPort
    $serialPort.PortName    = $Port
    $serialPort.BaudRate    = $BaudRate
    $serialPort.DataBits    = 8
    $serialPort.Parity      = [System.IO.Ports.Parity]::None
    $serialPort.StopBits    = [System.IO.Ports.StopBits]::One
    $serialPort.ReadTimeout = 2000
    $serialPort.Open()
    Write-Host "OK: $Port se connected!" -ForegroundColor Green
    Write-Host ""
} catch {
    Write-Host "ERROR: $Port nahi khula!" -ForegroundColor Red
    Write-Host "Check karo:" -ForegroundColor Yellow
    Write-Host "  1. ESP32 USB se connected hai?" -ForegroundColor Yellow
    Write-Host "  2. CardScanner.ino upload hua hai?" -ForegroundColor Yellow
    Write-Host "  3. Arduino IDE ka Serial Monitor band karo" -ForegroundColor Yellow
    Write-Host ""
    Read-Host "Press Enter to exit"
    exit 1
}

$lastUID = ""

try {
    while ($true) {
        try {
            $line = $serialPort.ReadLine().Trim()

            if ($line -match "^UID:(.+)$") {
                $uid = $matches[1].Trim()

                if ($uid -ne $lastUID) {
                    $lastUID = $uid
                    Write-Host "Card Scanned: $uid" -ForegroundColor Green

                    try {
                        $json = '{"uid":"' + $uid + '"}'
                        $response = Invoke-RestMethod -Uri "$Server/api/card-scan" `
                            -Method POST `
                            -Body $json `
                            -ContentType "application/json" `
                            -TimeoutSec 3

                        if ($response.success) {
                            Write-Host "  -> Web form mein fill ho gaya!" -ForegroundColor Cyan
                        }
                    } catch {
                        Write-Host "  -> Server tak nahi pahuncha. Server chal raha hai?" -ForegroundColor Yellow
                    }
                    Write-Host ""
                }
            }
        } catch [System.TimeoutException] {
            # Normal - koi card nahi tha
        } catch {
            # ignore
        }
    }
} finally {
    if ($serialPort -and $serialPort.IsOpen) {
        $serialPort.Close()
        Write-Host "Port band ho gaya." -ForegroundColor Gray
    }
}
