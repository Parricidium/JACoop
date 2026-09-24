@echo off
rem Rejoindre.cmd [ADRESSE[:PORT]] : sans adresse, le menu COOPERATION > REJOINDRE (liste LAN / adresse)
start "" "%~dp0JACoop.exe" join %*
