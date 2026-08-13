param(
    [string]$Port = "COM4",
    [int]$BaudRate = 115200,
    [int]$DurationSeconds = 8,
    [string]$OutputDirectory = "$PSScriptRoot\test-results"
)

$ErrorActionPreference = "Stop"

if ($DurationSeconds -lt 1) {
    throw "DurationSeconds must be at least 1."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $OutputDirectory "consumer-test-$timestamp.log"
$csvPath = Join-Path $OutputDirectory "consumer-test-$timestamp.csv"

$serialPort = [System.IO.Ports.SerialPort]::new(
    $Port,
    $BaudRate,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serialPort.ReadTimeout = 100
$serialPort.DtrEnable = $false
$serialPort.RtsEnable = $true

try {
    $serialPort.Open()

    # ESP32 EN 핀을 RTS로 짧게 Low로 만들어 테스트를 처음부터 다시 실행한다.
    Start-Sleep -Milliseconds 100
    $serialPort.RtsEnable = $false

    $deadline = (Get-Date).AddSeconds($DurationSeconds)
    $serialLog = ""
    while ((Get-Date) -lt $deadline) {
        $serialLog += $serialPort.ReadExisting()
        Start-Sleep -Milliseconds 50
    }
} finally {
    if ($serialPort.IsOpen) {
        $serialPort.Close()
    }
    $serialPort.Dispose()
}

[System.IO.File]::WriteAllText($logPath, $serialLog, [System.Text.UTF8Encoding]::new($false))

$records = @(
    foreach ($line in ($serialLog -split "`r?`n")) {
        $markerIndex = $line.IndexOf("TEST_CSV,")
        if ($markerIndex -lt 0) {
            continue
        }

        $csvBody = $line.Substring($markerIndex + "TEST_CSV,".Length)
        $csvBody | ConvertFrom-Csv -Header @(
            "timestamp_ms",
            "suite",
            "test_case",
            "status",
            "detail"
        )
    }
)

if ($records.Count -eq 0) {
    Write-Error "No TEST_CSV records received. Raw log: $logPath"
    exit 2
}

$records | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
$records | Format-Table timestamp_ms, test_case, status, detail -AutoSize

$failed = @($records | Where-Object status -ne "PASS")
Write-Host "Raw log: $logPath"
Write-Host "CSV:     $csvPath"
Write-Host "Result:  $($records.Count - $failed.Count)/$($records.Count) PASS"

if ($failed.Count -gt 0) {
    exit 1
}
