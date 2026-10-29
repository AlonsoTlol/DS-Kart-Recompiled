#!/bin/sh
# Genera el logo: geometría (genera_logo.py) -> PNG e icono.ico (Exporta.exe, WPF con el csc de .NET 4).
cd "$(dirname "$0")"
PY=/c/Users/Administrator/AppData/Local/Programs/Python/Python312/python.exe
FW=/c/Windows/Microsoft.NET/Framework64/v4.0.30319
"$PY" genera_logo.py || exit 1
"$FW/csc.exe" -nologo -target:winexe -optimize -out:Exporta.exe \
    -r:"$FW/WPF/PresentationFramework.dll" -r:"$FW/WPF/PresentationCore.dll" -r:"$FW/WPF/WindowsBase.dll" \
    -r:"$FW/System.Xaml.dll" Exporta.cs || exit 1
timeout 60 ./Exporta.exe logo_xaml.txt . && ls -la logo_512.png prueba.png icono.ico
"$FW/csc.exe" -nologo -target:winexe -optimize -out:Marca.exe \
    -r:"$FW/WPF/PresentationFramework.dll" -r:"$FW/WPF/PresentationCore.dll" -r:"$FW/WPF/WindowsBase.dll" \
    -r:"$FW/System.Xaml.dll" Marca.cs || exit 1
timeout 60 ./Marca.exe logo_xaml.txt . && ls -la logo_horizontal.png logo_horizontal.svg
