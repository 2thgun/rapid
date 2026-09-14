Option Explicit
Dim shell, fso, folder, nativePath, configPath, keyPath, dpapiPath, command
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
folder = fso.GetParentFolderName(WScript.ScriptFullName)
nativePath = fso.BuildPath(folder, "rapid-telemetry-daemon.exe")
If Not fso.FileExists(nativePath) Then
    MsgBox "The native raPId executable is missing." & vbCrLf & _
        "Extract the complete companion folder before starting, or build rapid-telemetry-daemon.exe.", _
        vbExclamation, "raPId could not start"
    WScript.Quit 1
End If
shell.CurrentDirectory = folder
command = """" & nativePath & """"
configPath = fso.BuildPath(folder, "daemon.conf")
keyPath = fso.BuildPath(folder, "telemetry.key")
dpapiPath = shell.ExpandEnvironmentStrings("%LOCALAPPDATA%\raPId\pairing.key.dpapi")
If fso.FileExists(configPath) Then command = command & " --config """ & configPath & """"
If fso.FileExists(dpapiPath) Then
    command = command & " --auth-key-dpapi-file """ & dpapiPath & """"
ElseIf fso.FileExists(keyPath) Then
    shell.Environment("Process")("RAPID_TELEMETRY_KEY") = ""
    command = command & " --auth-key-file """ & keyPath & """"
End If
shell.Run command, 0, False
