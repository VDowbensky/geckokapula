@echo off
setlocal enableextensions enabledelayedexpansion
SET allParams=%*
SET params=%allParams:' '=","%
SET params=%params:'="%
set /a count=0
for %%i in (%params%) do (
  set /a count+=1
  set param[!count!]=%%i
)

set "studioInstallationDir=/home/ubuntu/SimplicityStudio_v4"
set "projDir=%~dp0"
set "projDir=%projDir:"=%"

set "projDir=%projDir:\=/%"
set "projDir=%projDir:Z:=%"
start /unix "%studioInstallationDir%/jre/bin/java" -jar "/home/ubuntu/SimplicityStudio_v4/plugins/com.silabs.external.jython_2.7.0.201705012047-102/external_jython/2.7.0/jython-2.7.0.jar"  "%projDir%/mgkokeilu2-postbuild.py" %param[1]%  %param[2]%  "wine start /unix " "wine cmd /C" %param[3]% 