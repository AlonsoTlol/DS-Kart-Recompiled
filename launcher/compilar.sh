#!/bin/sh
# Compila el launcher con el csc de .NET Framework 4 (viene con Windows, no hace falta instalar nada).
# Deja Launcher.exe en la carpeta del proyecto (junto a build/ y ajustes.ini).
cd "$(dirname "$0")/.."
FW=/c/Windows/Microsoft.NET/Framework64/v4.0.30319
"$FW/csc.exe" -nologo -target:winexe -optimize -out:Launcher.exe -win32icon:logo/icono.ico \
    -r:"$FW/WPF/PresentationFramework.dll" -r:"$FW/WPF/PresentationCore.dll" -r:"$FW/WPF/WindowsBase.dll" \
    -r:"$FW/System.Xaml.dll" -r:"$FW/System.IO.Compression.dll" -r:"$FW/System.IO.Compression.FileSystem.dll" \
    "launcher\Launcher.cs" "launcher\Textos.cs" "launcher\Preparar.cs"
