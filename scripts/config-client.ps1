<#
.SYNOPSIS
  Interactive client for the module's TCP configurator (Config/Src/config_server.c,
  port 7000) - no PuTTY/netcat needed, just a raw TCP socket via .NET.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts/config-client.ps1 10.0.1.140

  Then just type commands at the prompt (SET SERVER, SET NAME, SET IP DHCP/STATIC,
  SAVE, GET) and read the OK/ERR reply after each one. Type "exit" or Ctrl+C to quit.
  REBOOT closes the connection on the module's end - that's expected, just re-run
  this script afterwards if you need to send more commands.
#>
param(
  [Parameter(Mandatory = $true)]
  [string]$ModuleIp,

  [int]$Port = 7000
)

$ErrorActionPreference = 'Stop'

function Read-Reply([System.Net.Sockets.NetworkStream]$Stream)
{
  Start-Sleep -Milliseconds 200  # give the module a moment to reply
  $buf = New-Object byte[] 1024
  $received = ''
  while ($Stream.DataAvailable)
  {
    $n = $Stream.Read($buf, 0, $buf.Length)
    if ($n -le 0) { break }
    $received += [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
  }
  return $received
}

Write-Host "Connecting to $ModuleIp`:$Port ..."
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect($ModuleIp, $Port)
$stream = $client.GetStream()
$stream.ReadTimeout = 3000

Write-Host (Read-Reply $stream).TrimEnd()
Write-Host "Type a command (SET SERVER/NAME/IP, SAVE, REBOOT, GET) or 'exit':"

while ($true)
{
  Write-Host -NoNewline "> "
  $line = Read-Host
  if ([string]::IsNullOrWhiteSpace($line) -or $line -eq 'exit') { break }

  $bytes = [System.Text.Encoding]::ASCII.GetBytes($line + "`r`n")
  try
  {
    $stream.Write($bytes, 0, $bytes.Length)
  }
  catch
  {
    Write-Host "(connection closed - module likely rebooted; re-run this script if you need to send more)"
    break
  }

  try
  {
    $reply = Read-Reply $stream
    if ($reply) { Write-Host $reply.TrimEnd() } else { Write-Host "(no reply)" }
  }
  catch
  {
    Write-Host "(no reply / connection closed - module likely rebooted)"
    break
  }
}

$client.Close()
