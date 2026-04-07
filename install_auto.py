import sys, subprocess, time

PIPE_NAME = r'np21w_com1'
cmd_exec = "exec install.bin\r"
cmd_y = "y"

byte_seq = list(cmd_exec.encode('ascii'))
time.sleep(1) # just in case
byte_array_1 = ','.join(str(b) for b in byte_seq)
byte_array_2 = str(ord('y'))

ps = (
    "$ErrorActionPreference = 'Stop'; "
    "try { "
    "  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', "
    f"'{PIPE_NAME}', [System.IO.Pipes.PipeDirection]::InOut); "
    "  $pipe.Connect(5000); "
    f"  [byte[]]$data1 = @({byte_array_1}); "
    f"  [byte[]]$data2 = @({byte_array_2}); "
    "  $pipe.Write($data1, 0, $data1.Length); "
    "  $pipe.Flush(); "
    "  Start-Sleep -Seconds 2; "
    "  $pipe.Write($data2, 0, $data2.Length); "
    "  $pipe.Flush(); "
    "  $buf = New-Object byte[] 1; "
    "  $sb = New-Object System.Text.StringBuilder; "
    "  $timeout = [DateTime]::Now.AddSeconds(40); "
    "  while ([DateTime]::Now -lt $timeout) { "
    "    if ($pipe.Read($buf, 0, 1) -gt 0) { "
    "      if ($buf[0] -eq 4) { break; } "
    "      [void]$sb.Append([char]$buf[0]); "
    "      $timeout = [DateTime]::Now.AddSeconds(5); "
    "    } "
    "  }; "
    "  Write-Host $sb.ToString(); "
    "  $pipe.Close(); "
    "} catch { Write-Host 'ERROR: ' $_ }"
)

print("Running automated install over serial...")
res = subprocess.run(['powershell.exe', '-NoProfile', '-Command', ps], capture_output=True, timeout=50)
print(res.stdout.decode('cp932', errors='replace'))
