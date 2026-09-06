param(
    [string]$PiHost = '255.255.255.255',
    [int]$PiPort = 9001,
    [ValidateRange(1, 100)][int]$SampleRate = 10,
    [string]$OutputDirectory = "$env:USERPROFILE\Documents\raPId Telemetry",
    [switch]$NoForward,
    [switch]$Headless,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Zeros([IO.BinaryWriter]$Writer, [int]$Count) {
    $Writer.Write([byte[]]::new($Count))
}

function Write-FixedString([IO.BinaryWriter]$Writer, [string]$Value, [int]$Length) {
    if ($null -eq $Value) { $Value = '' }
    $bytes = [Text.Encoding]::ASCII.GetBytes($Value)
    $count = [Math]::Min($bytes.Length, $Length)
    $Writer.Write($bytes, 0, $count)
    Write-Zeros $Writer ($Length - $count)
}

function Read-WideString($Accessor, [int]$Offset, [int]$Characters) {
    $bytes = [byte[]]::new($Characters * 2)
    [void]$Accessor.ReadArray($Offset, $bytes, 0, $bytes.Length)
    [Text.Encoding]::Unicode.GetString($bytes).TrimEnd([char]0)
}

function Read-AsciiString($Accessor, [int]$Offset, [int]$Length) {
    $bytes = [byte[]]::new($Length)
    [void]$Accessor.ReadArray($Offset, $bytes, 0, $bytes.Length)
    [Text.Encoding]::ASCII.GetString($bytes).TrimEnd([char]0)
}

function New-Channel([string]$Name, [string]$ShortName, [string]$Unit, [string]$Key, [float]$Scale = 1) {
    [pscustomobject]@{
        Name = $Name; ShortName = $ShortName; Unit = $Unit; Key = $Key; Scale = $Scale
        Samples = [Collections.Generic.List[float]]::new()
    }
}

function New-ChannelSet {
    [Collections.Generic.List[object]]@(
        (New-Channel 'Time' 'Time' 's' 'elapsed'),
        (New-Channel 'Throttle Position' 'Throttle' '%' 'throttle' 100),
        (New-Channel 'Brake Position' 'Brake' '%' 'brake' 100),
        (New-Channel 'Fuel Level' 'Fuel' 'l' 'fuel'),
        (New-Channel 'Gear' 'Gear' '' 'gear'),
        (New-Channel 'Engine RPM' 'RPM' 'rpm' 'rpm'),
        (New-Channel 'Steering Position' 'Steer' 'rad' 'steering_angle'),
        (New-Channel 'Ground Speed' 'Speed' 'km/h' 'speed_kmh'),
        (New-Channel 'Velocity X' 'Vel X' 'm/s' 'velocity_x'),
        (New-Channel 'Velocity Y' 'Vel Y' 'm/s' 'velocity_y'),
        (New-Channel 'Velocity Z' 'Vel Z' 'm/s' 'velocity_z'),
        (New-Channel 'G Force X' 'G X' 'g' 'g_x'),
        (New-Channel 'G Force Y' 'G Y' 'g' 'g_y'),
        (New-Channel 'G Force Z' 'G Z' 'g' 'g_z'),
        (New-Channel 'Wheel Slip FL' 'Slip FL' '' 'wheel_slip_fl'),
        (New-Channel 'Wheel Slip FR' 'Slip FR' '' 'wheel_slip_fr'),
        (New-Channel 'Wheel Slip RL' 'Slip RL' '' 'wheel_slip_rl'),
        (New-Channel 'Wheel Slip RR' 'Slip RR' '' 'wheel_slip_rr'),
        (New-Channel 'Tyre Pressure FL' 'Press FL' 'psi' 'pressure_fl'),
        (New-Channel 'Tyre Pressure FR' 'Press FR' 'psi' 'pressure_fr'),
        (New-Channel 'Tyre Pressure RL' 'Press RL' 'psi' 'pressure_rl'),
        (New-Channel 'Tyre Pressure RR' 'Press RR' 'psi' 'pressure_rr'),
        (New-Channel 'Wheel Speed FL' 'WhlSp FL' 'rad/s' 'wheel_speed_fl'),
        (New-Channel 'Wheel Speed FR' 'WhlSp FR' 'rad/s' 'wheel_speed_fr'),
        (New-Channel 'Wheel Speed RL' 'WhlSp RL' 'rad/s' 'wheel_speed_rl'),
        (New-Channel 'Wheel Speed RR' 'WhlSp RR' 'rad/s' 'wheel_speed_rr'),
        (New-Channel 'Tyre Core Temp FL' 'Core FL' 'C' 'core_temp_fl'),
        (New-Channel 'Tyre Core Temp FR' 'Core FR' 'C' 'core_temp_fr'),
        (New-Channel 'Tyre Core Temp RL' 'Core RL' 'C' 'core_temp_rl'),
        (New-Channel 'Tyre Core Temp RR' 'Core RR' 'C' 'core_temp_rr'),
        (New-Channel 'Suspension Travel FL' 'Susp FL' 'm' 'suspension_fl'),
        (New-Channel 'Suspension Travel FR' 'Susp FR' 'm' 'suspension_fr'),
        (New-Channel 'Suspension Travel RL' 'Susp RL' 'm' 'suspension_rl'),
        (New-Channel 'Suspension Travel RR' 'Susp RR' 'm' 'suspension_rr'),
        (New-Channel 'TC Activity' 'TC' '' 'tc'),
        (New-Channel 'Heading' 'Heading' 'rad' 'heading'),
        (New-Channel 'Pitch' 'Pitch' 'rad' 'pitch'),
        (New-Channel 'Roll' 'Roll' 'rad' 'roll'),
        (New-Channel 'Damage Front' 'Dmg F' '' 'damage_front'),
        (New-Channel 'Damage Rear' 'Dmg R' '' 'damage_rear'),
        (New-Channel 'Damage Left' 'Dmg L' '' 'damage_left'),
        (New-Channel 'Damage Right' 'Dmg Rgt' '' 'damage_right'),
        (New-Channel 'Damage Center' 'Dmg C' '' 'damage_center'),
        (New-Channel 'Pit Limiter' 'Pit Lim' '' 'pit_limiter'),
        (New-Channel 'ABS Activity' 'ABS' '' 'abs'),
        (New-Channel 'Lap Number' 'Lap' '' 'lap_number'),
        (New-Channel 'Lap Time' 'Lap Time' 's' 'current_lap_ms' 0.001),
        (New-Channel 'Lap Position' 'Lap Pos' '%' 'lap_position' 100)
    )
}

function New-Frame {
    $frame = @{}
    foreach ($channel in (New-ChannelSet)) {
        if ($channel.Key -ne 'elapsed') { $frame[$channel.Key] = [float]0 }
    }
    $frame['completed_lap_ms'] = [int]0
    $frame['delta_ms'] = [int]0
    $frame
}

function Add-Sample($Channels, $Frame, [Diagnostics.Stopwatch]$Clock) {
    foreach ($channel in $Channels) {
        $value = if ($channel.Key -eq 'elapsed') {
            $Clock.Elapsed.TotalSeconds
        } elseif ($Frame.ContainsKey($channel.Key)) {
            [double]$Frame[$channel.Key] * [double]$channel.Scale
        } else { 0 }
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) { $value = 0 }
        $channel.Samples.Add([float]$value)
    }
}

function Write-MotecLd([string]$Path, $Channels, $Metadata) {
    if ($Channels.Count -eq 0 -or $Channels[0].Samples.Count -eq 0) {
        throw 'Cannot write an LD file without samples.'
    }
    $headerSize = 1762; $eventSize = 1154; $channelHeaderSize = 124
    $eventPointer = $headerSize
    $metadataPointer = $headerSize + $eventSize
    $dataPointer = $metadataPointer + ($Channels.Count * $channelHeaderSize)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $writer = [IO.BinaryWriter]::new($stream, [Text.Encoding]::ASCII, $false)
    try {
        $writer.Write([uint32]0x40); Write-Zeros $writer 4
        $writer.Write([uint32]$metadataPointer); $writer.Write([uint32]$dataPointer)
        Write-Zeros $writer 20; $writer.Write([uint32]$eventPointer); Write-Zeros $writer 24
        $writer.Write([uint16]1); $writer.Write([uint16]0x4240); $writer.Write([uint16]0x000f)
        $writer.Write([uint32]0x1f44); Write-FixedString $writer 'ADL' 8
        $writer.Write([uint16]420); $writer.Write([uint16]0xadb0)
        $writer.Write([uint32]$Channels.Count); Write-Zeros $writer 4
        Write-FixedString $writer $Metadata.StartedAt.ToString('dd/MM/yyyy') 16
        Write-Zeros $writer 16
        Write-FixedString $writer $Metadata.StartedAt.ToString('HH:mm:ss') 16
        Write-Zeros $writer 16
        Write-FixedString $writer $Metadata.Driver 64
        Write-FixedString $writer $Metadata.Vehicle 64
        Write-Zeros $writer 64
        Write-FixedString $writer $Metadata.Venue 64
        Write-Zeros $writer 64; Write-Zeros $writer 1024
        $writer.Write([uint32]0x000c81a4); Write-Zeros $writer 66
        Write-FixedString $writer "raPId $($Metadata.Simulator) telemetry" 64
        Write-Zeros $writer 126
        Write-FixedString $writer $Metadata.Simulator 64
        Write-FixedString $writer $Metadata.Session 64
        Write-FixedString $writer 'Recorded by raPId' 1024
        $writer.Write([uint16]0)

        $nextDataPointer = $dataPointer
        for ($i = 0; $i -lt $Channels.Count; $i++) {
            $channel = $Channels[$i]
            $previous = if ($i -eq 0) { 0 } else { $metadataPointer + (($i - 1) * $channelHeaderSize) }
            $next = if ($i -eq $Channels.Count - 1) { 0 } else { $metadataPointer + (($i + 1) * $channelHeaderSize) }
            $writer.Write([uint32]$previous); $writer.Write([uint32]$next)
            $writer.Write([uint32]$nextDataPointer); $writer.Write([uint32]$channel.Samples.Count)
            $writer.Write([uint16](0x2ee1 + $i)); $writer.Write([uint16]0x07)
            $writer.Write([uint16]4); $writer.Write([uint16]$SampleRate)
            $writer.Write([int16]0); $writer.Write([int16]1); $writer.Write([int16]1); $writer.Write([int16]0)
            Write-FixedString $writer $channel.Name 32
            Write-FixedString $writer $channel.ShortName 8
            Write-FixedString $writer $channel.Unit 12
            Write-Zeros $writer 40
            $nextDataPointer += $channel.Samples.Count * 4
        }
        foreach ($channel in $Channels) {
            foreach ($sample in $channel.Samples) { $writer.Write([float]$sample) }
        }
    } finally { $writer.Dispose() }
}

function Save-TelemetrySession($Channels, $Metadata, [string]$Directory) {
    if ($null -eq $Channels -or $Channels.Count -eq 0 -or $Channels[0].Samples.Count -eq 0) { return $null }
    $safeVenue = ($Metadata.Venue -replace '[^A-Za-z0-9_-]', '_').Trim('_')
    if (-not $safeVenue) { $safeVenue = $Metadata.Simulator }
    $name = '{0:yyyy-MM-dd_HH-mm-ss}_{1}_{2}.ld' -f $Metadata.StartedAt, $Metadata.Simulator, $safeVenue
    $path = Join-Path $Directory $name
    Write-MotecLd $path $Channels $Metadata
    $path
}

function New-Metadata([string]$Simulator, [string]$Driver = '', [string]$Vehicle = '', [string]$Venue = '', [string]$Session = '') {
    [pscustomobject]@{
        StartedAt = [datetime]::Now; Simulator = $Simulator; Driver = $Driver
        Vehicle = $Vehicle; Venue = $Venue; Session = $Session
    }
}

function Get-RunningAssettoSimulator {
    $names = @((Get-Process -ErrorAction SilentlyContinue).ProcessName)
    if ($names -contains 'AC2-Win64-Shipping') { return 'ACC' }
    if ($names -contains 'acc') { return 'ACC' }
    if ($names -contains 'acs') { return 'AC' }
    'AC'
}

function Open-AssettoAdapter {
    $physicsMap = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\acpmf_physics')
    try {
        $graphicsMap = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\acpmf_graphics')
        $staticMap = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\acpmf_static')
    } catch {
        $physicsMap.Dispose(); throw
    }
    $physics = $physicsMap.CreateViewAccessor(); $graphics = $graphicsMap.CreateViewAccessor(); $static = $staticMap.CreateViewAccessor()
    $simulator = Get-RunningAssettoSimulator
    $sessionType = $graphics.ReadInt32(8)
    $sessionNames = @('Practice','Qualifying','Race','Hotlap','Time Attack','Drift','Drag','Hotstint','Superpole')
    $session = if ($sessionType -ge 0 -and $sessionType -lt $sessionNames.Count) { $sessionNames[$sessionType] } else { $simulator }
    $driver = ((Read-WideString $static 200 33) + ' ' + (Read-WideString $static 266 33)).Trim()
    [pscustomobject]@{
        Kind='Assetto'; Simulator=$simulator; PhysicsMap=$physicsMap; GraphicsMap=$graphicsMap; StaticMap=$staticMap
        Physics=$physics; Graphics=$graphics; Static=$static
        Metadata=(New-Metadata $simulator $driver (Read-WideString $static 68 33) (Read-WideString $static 134 33) $session)
        LastLapMs=0; LastCurrentLapMs=0; SyntheticLap=1
    }
}

function Open-AceAdapter {
    $physicsMap = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\acevo_pmf_physics')
    try {
        $graphicsMap = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\acevo_pmf_graphics')
        $staticMap = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\acevo_pmf_static')
    } catch {
        $physicsMap.Dispose(); throw
    }
    $physics = $physicsMap.CreateViewAccessor(); $graphics = $graphicsMap.CreateViewAccessor(); $static = $staticMap.CreateViewAccessor()
    $sessionType = $static.ReadInt32(32)
    $sessionNames = @('Unknown','Practice','Qualifying','Race','Hotlap','Time Attack','Drift','Drag')
    $session = if ($sessionType -ge 0 -and $sessionType -lt $sessionNames.Count) { $sessionNames[$sessionType] } else { Read-AsciiString $static 36 33 }
    [pscustomobject]@{
        Kind='ACE'; Simulator='ACE'; PhysicsMap=$physicsMap; GraphicsMap=$graphicsMap; StaticMap=$staticMap
        Physics=$physics; Graphics=$graphics; Static=$static
        Metadata=(New-Metadata 'ACE' '' 'Assetto Corsa EVO car' 'Assetto Corsa EVO' $session)
        LastLapMs=0; LastCurrentLapMs=0; SyntheticLap=1
    }
}

function Get-IracingVariableTable($Accessor) {
    $count = $Accessor.ReadInt32(24); $offset = $Accessor.ReadInt32(28)
    if ($count -lt 1 -or $count -gt 4096 -or $offset -lt 0) { throw 'Invalid iRacing variable table.' }
    $variables = @{}
    for ($i = 0; $i -lt $count; $i++) {
        $at = $offset + ($i * 144)
        $name = Read-AsciiString $Accessor ($at + 16) 32
        if ($name) {
            $variables[$name] = [pscustomobject]@{ Type=$Accessor.ReadInt32($at); Offset=$Accessor.ReadInt32($at + 4); Count=$Accessor.ReadInt32($at + 8) }
        }
    }
    $variables
}

function Get-YamlValue([string]$Yaml, [string]$Name, [string]$Default = '') {
    $pattern = '(?m)^\s*{0}:\s*[''"]?([^"''\r\n]+)' -f [regex]::Escape($Name)
    $match = [regex]::Match($Yaml, $pattern)
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    $Default
}

function Open-IracingAdapter {
    $map = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\IRSDKMemMapFileName')
    $accessor = $map.CreateViewAccessor()
    try {
        if (($accessor.ReadInt32(4) -band 1) -eq 0) { throw 'iRacing mapping is not connected.' }
        $variables = Get-IracingVariableTable $accessor
        $yamlLength = $accessor.ReadInt32(16); $yamlOffset = $accessor.ReadInt32(20)
        $yaml = if ($yamlLength -gt 0 -and $yamlLength -lt 10MB) { Read-AsciiString $accessor $yamlOffset $yamlLength } else { '' }
        $metadata = New-Metadata 'iRacing' (Get-YamlValue $yaml 'UserName') (Get-YamlValue $yaml 'CarScreenName') (Get-YamlValue $yaml 'TrackDisplayName') (Get-YamlValue $yaml 'SessionType' 'iRacing')
        [pscustomobject]@{ Kind='iRacing'; Simulator='iRacing'; Map=$map; Accessor=$accessor; Variables=$variables; Metadata=$metadata; LastTick=-1 }
    } catch { $accessor.Dispose(); $map.Dispose(); throw }
}

function Open-AvailableAdapter {
    try { return Open-AceAdapter } catch [IO.FileNotFoundException] { }
    try { return Open-IracingAdapter } catch [IO.FileNotFoundException] { }
    try { return Open-AssettoAdapter } catch [IO.FileNotFoundException] { }
    $null
}

function Close-Adapter($Adapter) {
    if ($null -eq $Adapter) { return }
    foreach ($name in @('Physics','Graphics','Static','Accessor','PhysicsMap','GraphicsMap','StaticMap','Map')) {
        if ($Adapter.PSObject.Properties.Name -contains $name -and $null -ne $Adapter.$name) {
            try { $Adapter.$name.Dispose() } catch { }
        }
    }
}

function Read-AssettoFrame($Adapter) {
    $p = $Adapter.Physics; $g = $Adapter.Graphics
    $before = $p.ReadInt32(0)
    $frame = New-Frame
    $frame.throttle=$p.ReadSingle(4); $frame.brake=$p.ReadSingle(8); $frame.fuel=$p.ReadSingle(12)
    $frame.gear=$p.ReadInt32(16)-1; $frame.rpm=$p.ReadInt32(20); $frame.steering_angle=$p.ReadSingle(24); $frame.speed_kmh=$p.ReadSingle(28)
    $frame.velocity_x=$p.ReadSingle(32); $frame.velocity_y=$p.ReadSingle(36); $frame.velocity_z=$p.ReadSingle(40)
    $frame.g_x=$p.ReadSingle(44); $frame.g_y=$p.ReadSingle(48); $frame.g_z=$p.ReadSingle(52)
    $keys = @('wheel_slip','pressure','wheel_speed','core_temp','suspension')
    $offsets = @(56,88,104,152,184); $corners = @('fl','fr','rl','rr')
    for ($group=0; $group -lt $keys.Count; $group++) { for ($i=0; $i -lt 4; $i++) { $frame["$($keys[$group])_$($corners[$i])"]=$p.ReadSingle($offsets[$group]+($i*4)) } }
    $frame.tc=$p.ReadSingle(204); $frame.heading=$p.ReadSingle(208); $frame.pitch=$p.ReadSingle(212); $frame.roll=$p.ReadSingle(216)
    for ($i=0; $i -lt 5; $i++) { $frame[@('damage_front','damage_rear','damage_left','damage_right','damage_center')[$i]]=$p.ReadSingle(224+($i*4)) }
    $frame.pit_limiter=$p.ReadInt32(248); $frame.abs=$p.ReadSingle(252)
    $frame.lap_number=$g.ReadInt32(132)+1; $frame.current_lap_ms=[Math]::Max(0,$g.ReadInt32(140)); $frame.completed_lap_ms=$g.ReadInt32(144); $frame.lap_position=$g.ReadSingle(248)
    if ($frame.completed_lap_ms -le 0 -or $frame.completed_lap_ms -eq [int]::MaxValue) { $frame.completed_lap_ms=0 }
    if ($before -ne $p.ReadInt32(0)) { return $null }
    $frame
}

function Read-AceFrame($Adapter) {
    $p = $Adapter.Physics; $g = $Adapter.Graphics
    $before = $p.ReadInt32(0); $frame = New-Frame
    $frame.throttle=$p.ReadSingle(4); $frame.brake=$p.ReadSingle(8); $frame.fuel=$p.ReadSingle(12)
    $frame.gear=$p.ReadInt32(16)-1; $frame.rpm=$p.ReadInt32(20); $frame.steering_angle=$p.ReadSingle(24); $frame.speed_kmh=$p.ReadSingle(28)
    $frame.velocity_x=$p.ReadSingle(32); $frame.velocity_y=$p.ReadSingle(36); $frame.velocity_z=$p.ReadSingle(40)
    $frame.g_x=$p.ReadSingle(44); $frame.g_y=$p.ReadSingle(48); $frame.g_z=$p.ReadSingle(52)
    $keys = @('wheel_slip','pressure','wheel_speed','core_temp','suspension'); $offsets = @(56,88,104,152,184); $corners = @('fl','fr','rl','rr')
    for ($group=0; $group -lt $keys.Count; $group++) { for ($i=0; $i -lt 4; $i++) { $frame["$($keys[$group])_$($corners[$i])"]=$p.ReadSingle($offsets[$group]+($i*4)) } }
    $frame.tc=$p.ReadSingle(204); $frame.heading=$p.ReadSingle(208); $frame.pitch=$p.ReadSingle(212); $frame.roll=$p.ReadSingle(216)
    $frame.current_lap_ms=[Math]::Max(0,$g.ReadInt32(188)); $frame.delta_ms=$g.ReadInt32(184); $frame.lap_position=$g.ReadSingle(1244)
    if ($Adapter.LastCurrentLapMs -gt 15000 -and $frame.current_lap_ms -lt 2000) { $Adapter.LastLapMs=$Adapter.LastCurrentLapMs; $Adapter.SyntheticLap++ }
    $Adapter.LastCurrentLapMs=$frame.current_lap_ms; $frame.completed_lap_ms=$Adapter.LastLapMs; $frame.lap_number=$Adapter.SyntheticLap
    $frame.tc=$p.ReadInt32(672); $frame.abs=$p.ReadInt32(676)
    if ($before -ne $p.ReadInt32(0)) { return $null }
    $frame
}

function Get-IracingBufferOffset($Adapter) {
    $accessor = $Adapter.Accessor; $count = $accessor.ReadInt32(32)
    $bestTick = [int]::MinValue; $bestOffset = -1
    for ($i=0; $i -lt [Math]::Min($count,4); $i++) {
        $at = 48 + ($i*16); $tick=$accessor.ReadInt32($at)
        if ($tick -gt $bestTick) { $bestTick=$tick; $bestOffset=$accessor.ReadInt32($at+4) }
    }
    if ($bestOffset -lt 0) { throw 'iRacing has no readable telemetry buffer.' }
    $Adapter.LastTick=$bestTick
    $bestOffset
}

function Read-IracingValue($Adapter, [int]$BufferOffset, [string]$Name, $Default = 0) {
    if (-not $Adapter.Variables.ContainsKey($Name)) { return $Default }
    $variable=$Adapter.Variables[$Name]; $at=$BufferOffset+$variable.Offset
    switch ($variable.Type) {
        0 { return [int]$Adapter.Accessor.ReadByte($at) }
        1 { return ($Adapter.Accessor.ReadByte($at) -ne 0) }
        2 { return $Adapter.Accessor.ReadInt32($at) }
        3 { return $Adapter.Accessor.ReadInt32($at) }
        4 { return $Adapter.Accessor.ReadSingle($at) }
        5 { return $Adapter.Accessor.ReadDouble($at) }
        default { return $Default }
    }
}

function Read-IracingFrame($Adapter) {
    if (($Adapter.Accessor.ReadInt32(4) -band 1) -eq 0) { throw 'iRacing disconnected.' }
    $offset=Get-IracingBufferOffset $Adapter; $frame=New-Frame; $g=9.80665
    $frame.throttle=Read-IracingValue $Adapter $offset 'Throttle'; $frame.brake=Read-IracingValue $Adapter $offset 'Brake'; $frame.fuel=Read-IracingValue $Adapter $offset 'FuelLevel'
    $frame.gear=Read-IracingValue $Adapter $offset 'Gear'; $frame.rpm=Read-IracingValue $Adapter $offset 'RPM'; $frame.steering_angle=Read-IracingValue $Adapter $offset 'SteeringWheelAngle'
    $frame.speed_kmh=(Read-IracingValue $Adapter $offset 'Speed')*3.6
    $frame.velocity_x=Read-IracingValue $Adapter $offset 'VelocityX'; $frame.velocity_y=Read-IracingValue $Adapter $offset 'VelocityY'; $frame.velocity_z=Read-IracingValue $Adapter $offset 'VelocityZ'
    $frame.g_x=(Read-IracingValue $Adapter $offset 'LatAccel')/$g; $frame.g_y=(Read-IracingValue $Adapter $offset 'VertAccel')/$g; $frame.g_z=(Read-IracingValue $Adapter $offset 'LongAccel')/$g
    $frame.lap_number=Read-IracingValue $Adapter $offset 'Lap'; $frame.current_lap_ms=[Math]::Max(0,[int]((Read-IracingValue $Adapter $offset 'LapCurrentLapTime')*1000))
    $frame.completed_lap_ms=[Math]::Max(0,[int]((Read-IracingValue $Adapter $offset 'LapLastLapTime')*1000)); $frame.delta_ms=[int]((Read-IracingValue $Adapter $offset 'LapDeltaToBestLap')*1000)
    $frame.lap_position=Read-IracingValue $Adapter $offset 'LapDistPct'; $frame.pit_limiter=((Read-IracingValue $Adapter $offset 'EngineWarnings') -band 0x10) -ne 0
    $shockNames=@('LFshockDefl','RFshockDefl','LRshockDefl','RRshockDefl'); $corners=@('fl','fr','rl','rr')
    for ($i=0;$i -lt 4;$i++) { $frame["suspension_$($corners[$i])"]=Read-IracingValue $Adapter $offset $shockNames[$i] }
    $frame
}

function Test-AdapterLive($Adapter) {
    switch ($Adapter.Kind) {
        'Assetto' { return ($Adapter.Graphics.ReadInt32(4) -eq 2) }
        'ACE' { return ($Adapter.Graphics.ReadInt32(4) -eq 2) }
        'iRacing' {
            $offset=Get-IracingBufferOffset $Adapter
            return [bool](Read-IracingValue $Adapter $offset 'IsOnTrack' (Read-IracingValue $Adapter $offset 'IsOnTrackCar' $false))
        }
    }
    $false
}

function Read-AdapterFrame($Adapter) {
    switch ($Adapter.Kind) {
        'Assetto' { Read-AssettoFrame $Adapter }
        'ACE' { Read-AceFrame $Adapter }
        'iRacing' { Read-IracingFrame $Adapter }
    }
}

function Test-LdWriter {
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $channels=New-ChannelSet; $frame=New-Frame; $frame.rpm=6123; $frame.speed_kmh=201.5
    $clock=[Diagnostics.Stopwatch]::StartNew(); Add-Sample $channels $frame $clock; Add-Sample $channels $frame $clock
    $metadata=New-Metadata 'SELFTEST' 'Test Driver' 'Test Car' 'Test Track' 'Test'
    $path=Join-Path $OutputDirectory 'rapid-self-test.ld'; Write-MotecLd $path $channels $metadata
    $bytes=[IO.File]::ReadAllBytes($path); $meta=[BitConverter]::ToUInt32($bytes,8); $data=[BitConverter]::ToUInt32($bytes,12); $count=[BitConverter]::ToUInt32($bytes,86)
    if ([BitConverter]::ToUInt32($bytes,0) -ne 0x40 -or $count -ne $channels.Count) { throw 'LD header self-test failed.' }
    $pointer=$meta
    for ($i=0;$i -lt $count;$i++) {
        $previous=[BitConverter]::ToUInt32($bytes,$pointer); $next=[BitConverter]::ToUInt32($bytes,$pointer+4)
        $channelData=[BitConverter]::ToUInt32($bytes,$pointer+8); $samples=[BitConverter]::ToUInt32($bytes,$pointer+12)
        if (($i -eq 0 -and $previous -ne 0) -or $channelData -lt $data -or $samples -ne 2) { throw "LD channel self-test failed at $i." }
        if ($i -eq $count-1) { if ($next -ne 0) { throw 'LD channel chain is not terminated.' } } elseif ($next -ne $pointer+124) { throw "LD next pointer failed at $i." }
        $pointer=$next
    }
    Write-Output "raPId daemon self-test passed: $path"
}

if ($SelfTest) { Test-LdWriter; return }

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$script:LogPath=Join-Path $OutputDirectory 'rapid-daemon.log'
function Write-DaemonLog([string]$Message) {
    $line='{0:u} {1}' -f [datetime]::Now,$Message
    [IO.File]::AppendAllText($script:LogPath,$line+[Environment]::NewLine)
    if ($Headless) { Write-Host $line }
}

$created=$false; $script:Mutex=[Threading.Mutex]::new($true,'Local\raPIdTelemetryDaemon',[ref]$created)
if (-not $created) { if (-not $Headless) { Add-Type -AssemblyName System.Windows.Forms; [Windows.Forms.MessageBox]::Show('raPId telemetry daemon is already running.','raPId') | Out-Null }; return }

$script:Udp=if ($NoForward) { $null } else {
    $client=[Net.Sockets.UdpClient]::new()
    $client.EnableBroadcast=$true
    $client
}
$script:Adapter=$null; $script:Channels=$null; $script:Metadata=$null; $script:Clock=[Diagnostics.Stopwatch]::new()
$script:Recording=$false; $script:InactiveSince=$null; $script:LastProbe=[datetime]::MinValue; $script:LastLog=''; $script:LastError='None'
$script:PacketsSent=0; $script:SampleCount=0; $script:Status='Waiting for a supported simulator'; $script:Stopping=$false
$script:LastHeartbeat=[datetime]::MinValue

function Send-StatusHeartbeat {
    if ($null -eq $script:Udp -or ([datetime]::Now-$script:LastHeartbeat).TotalMilliseconds -lt 1000) { return }
    $state=if ($script:Recording) { 'driving' } elseif ($null -ne $script:Adapter) { 'ready' } else { 'waiting' }
    $simulator=if ($null -ne $script:Adapter) { $script:Adapter.Simulator } else { $null }
    $message=[ordered]@{ version=3; type='status'; state=$state; simulator=$simulator; sample_rate_hz=$SampleRate } | ConvertTo-Json -Compress
    $bytes=[Text.Encoding]::UTF8.GetBytes($message)
    [void]$script:Udp.Send($bytes,$bytes.Length,$PiHost,$PiPort)
    $script:PacketsSent++; $script:LastHeartbeat=[datetime]::Now
}

function Send-LiveFrame($Frame,$Adapter) {
    if ($null -eq $script:Udp) { return }
    $messageData=[ordered]@{
        version=3; type='telemetry'; simulator=$Adapter.Simulator; sample_rate_hz=$SampleRate
        track_name=$Adapter.Metadata.Venue; car_model=$Adapter.Metadata.Vehicle; driver_name=$Adapter.Metadata.Driver
        session_name=$Adapter.Metadata.Session; telemetry=$Frame
    }
    $message=$messageData | ConvertTo-Json -Compress -Depth 4
    $bytes=[Text.Encoding]::UTF8.GetBytes($message)
    [void]$script:Udp.Send($bytes,$bytes.Length,$PiHost,$PiPort); $script:PacketsSent++
}

function Finish-Recording {
    if (-not $script:Recording) { return }
    try {
        $path=Save-TelemetrySession $script:Channels $script:Metadata $OutputDirectory
        if ($path) { $script:LastLog=$path; Write-DaemonLog "Saved $script:SampleCount samples to $path" }
    } catch { $script:LastError=$_.Exception.Message; Write-DaemonLog "Save failed: $script:LastError" }
    $script:Recording=$false; $script:Channels=$null; $script:Metadata=$null; $script:SampleCount=0; $script:Clock.Reset()
}

function Start-Recording($Adapter) {
    $script:Channels=New-ChannelSet
    $script:Metadata=New-Metadata $Adapter.Simulator $Adapter.Metadata.Driver $Adapter.Metadata.Vehicle $Adapter.Metadata.Venue $Adapter.Metadata.Session
    $script:Clock.Restart(); $script:SampleCount=0; $script:Recording=$true; $script:InactiveSince=$null
    Write-DaemonLog "Recording $($Adapter.Simulator): $($Adapter.Metadata.Vehicle) at $($Adapter.Metadata.Venue)"
}

function Invoke-DaemonTick {
    if ($script:Stopping) { return }
    try {
        Send-StatusHeartbeat
        if ($null -eq $script:Adapter) {
            if (([datetime]::Now-$script:LastProbe).TotalMilliseconds -lt 1000) { return }
            $script:LastProbe=[datetime]::Now; $script:Adapter=Open-AvailableAdapter
            if ($null -eq $script:Adapter) { $script:Status='Waiting for ACC, AC, ACE, or iRacing'; return }
            $script:Status="$($script:Adapter.Simulator) detected - waiting for driving"
            Write-DaemonLog "$($script:Adapter.Simulator) telemetry detected"
        }
        $isLive=Test-AdapterLive $script:Adapter
        if ($isLive) {
            $script:InactiveSince=$null
            if (-not $script:Recording) { Start-Recording $script:Adapter }
            $frame=Read-AdapterFrame $script:Adapter
            if ($null -eq $frame) { return }
            Add-Sample $script:Channels $frame $script:Clock; $script:SampleCount++
            Send-LiveFrame $frame $script:Adapter
            $script:Status="$($script:Adapter.Simulator) recording - $script:SampleCount samples"
        } else {
            $script:Status="$($script:Adapter.Simulator) connected - waiting for driving"
            if ($script:Recording) {
                if ($null -eq $script:InactiveSince) { $script:InactiveSince=[datetime]::Now }
                elseif (([datetime]::Now-$script:InactiveSince).TotalSeconds -ge 2) { Finish-Recording }
            }
        }
        $script:LastError='None'
    } catch [IO.FileNotFoundException] {
        Finish-Recording; Close-Adapter $script:Adapter; $script:Adapter=$null; $script:Status='Waiting for a supported simulator'
    } catch {
        $script:LastError=$_.Exception.Message; Write-DaemonLog "Telemetry disconnected: $script:LastError"
        Finish-Recording; Close-Adapter $script:Adapter; $script:Adapter=$null; $script:Status='Waiting for a supported simulator'
    }
}

function Stop-Daemon {
    $script:Stopping=$true; Finish-Recording; Close-Adapter $script:Adapter
    if ($null -ne $script:Udp) { $script:Udp.Dispose() }
    if ($null -ne $script:Mutex) { try { $script:Mutex.ReleaseMutex() } catch { }; $script:Mutex.Dispose() }
}

Write-DaemonLog "Daemon started; sample rate $SampleRate Hz; Pi $PiHost`:$PiPort"
if ($Headless) {
    try { while ($true) { Invoke-DaemonTick; Start-Sleep -Milliseconds ([Math]::Max(1,[int](1000/$SampleRate))) } } finally { Stop-Daemon }
    return
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$form=[Windows.Forms.Form]::new(); $form.Text='raPId Telemetry'; $form.Size=[Drawing.Size]::new(520,330); $form.FormBorderStyle='FixedDialog'; $form.MaximizeBox=$false; $form.StartPosition='CenterScreen'; $form.ShowInTaskbar=$false
$title=[Windows.Forms.Label]::new(); $title.Text='raPId Telemetry Daemon'; $title.Font=[Drawing.Font]::new('Segoe UI',16,[Drawing.FontStyle]::Bold); $title.Location=[Drawing.Point]::new(22,18); $title.AutoSize=$true; $form.Controls.Add($title)
$labels=@{}
$rows=@(@('Status','Status'),@('Simulator','Simulator'),@('Forwarding','Forwarding'),@('Samples','Samples'),@('Last log','LastLog'),@('Last error','LastError'))
for ($i=0;$i -lt $rows.Count;$i++) {
    $left=[Windows.Forms.Label]::new(); $left.Text=$rows[$i][0]; $left.Location=[Drawing.Point]::new(24,65+($i*34)); $left.Size=[Drawing.Size]::new(100,24); $left.Font=[Drawing.Font]::new('Segoe UI',9,[Drawing.FontStyle]::Bold); $form.Controls.Add($left)
    $right=[Windows.Forms.Label]::new(); $right.Location=[Drawing.Point]::new(128,65+($i*34)); $right.Size=[Drawing.Size]::new(360,30); $right.AutoEllipsis=$true; $right.Font=[Drawing.Font]::new('Segoe UI',9); $form.Controls.Add($right); $labels[$rows[$i][1]]=$right
}
$openButton=[Windows.Forms.Button]::new(); $openButton.Text='Open telemetry folder'; $openButton.Location=[Drawing.Point]::new(320,260); $openButton.Size=[Drawing.Size]::new(168,30); $openButton.Add_Click({ Start-Process explorer.exe -ArgumentList $OutputDirectory }); $form.Controls.Add($openButton)
$form.Add_FormClosing({ param($sender,$eventArgs); if (-not $script:Stopping) { $eventArgs.Cancel=$true; $sender.Hide() } })

$menu=[Windows.Forms.ContextMenuStrip]::new(); $showItem=$menu.Items.Add('Show status'); $folderItem=$menu.Items.Add('Open telemetry folder'); [void]$menu.Items.Add('-'); $exitItem=$menu.Items.Add('Exit')
$notify=[Windows.Forms.NotifyIcon]::new(); $notify.Icon=[Drawing.SystemIcons]::Application; $notify.Text='raPId - waiting for simulator'; $notify.Visible=$true; $notify.ContextMenuStrip=$menu
$showStatus={ $form.Show(); $form.WindowState='Normal'; $form.Activate() }
$showItem.Add_Click($showStatus); $folderItem.Add_Click({ Start-Process explorer.exe -ArgumentList $OutputDirectory }); $notify.Add_MouseClick({ param($sender,$eventArgs); if ($eventArgs.Button -eq [Windows.Forms.MouseButtons]::Left) { & $showStatus } })
$exitItem.Add_Click({ $script:Stopping=$true; $form.Close(); [Windows.Forms.Application]::Exit() })

$timer=[Windows.Forms.Timer]::new(); $timer.Interval=[Math]::Max(10,[int](1000/$SampleRate))
$timer.Add_Tick({
    Invoke-DaemonTick
    $labels.Status.Text=$script:Status
    $labels.Simulator.Text=if ($null -eq $script:Adapter) { 'None detected' } else { $script:Adapter.Simulator }
    $labels.Forwarding.Text=if ($NoForward) { 'Disabled' } else { "$script:PacketsSent packets sent to $PiHost`:$PiPort" }
    $labels.Samples.Text=if ($script:Recording) { "$script:SampleCount in current recording" } else { 'Not recording' }
    $labels.LastLog.Text=if ($script:LastLog) { $script:LastLog } else { 'None yet' }
    $labels.LastError.Text=$script:LastError
    $tip="raPId - $script:Status"; if ($tip.Length -gt 63) { $tip=$tip.Substring(0,63) }; $notify.Text=$tip
})

try { $timer.Start(); [Windows.Forms.Application]::Run() } finally { $timer.Stop(); $timer.Dispose(); $notify.Visible=$false; $notify.Dispose(); Stop-Daemon }
