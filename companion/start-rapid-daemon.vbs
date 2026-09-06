Option Explicit
Dim shell, fso, folder, nativePath, scriptPath, command
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
folder = fso.GetParentFolderName(WScript.ScriptFullName)
nativePath = fso.BuildPath(folder, "rapid-telemetry-daemon.exe")
scriptPath = fso.BuildPath(folder, "rapid-telemetry-daemon.ps1")
If fso.FileExists(nativePath) Then
    command = """" & nativePath & """"
Else
    command = "powershell.exe -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File """ & scriptPath & """"
End If
shell.Run command, 0, False
