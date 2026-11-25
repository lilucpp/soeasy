; Copyright 2026 SoEasy Team. All rights reserved.
; NSIS Installer Script for SoEasy Desktop Search Engine (Windows x64)
; Build example:
;   makensis -DVERSION=0.4.4 -DSTAGE_DIR=..\..\dist\soeasy-v0.4.4-windows-x64 -DOUT_FILE=..\..\dist\soeasy-v0.4.4-windows-x64-setup.exe packaging\windows\soeasy.nsi

Unicode true

!ifndef VERSION
  !define VERSION "0.4.4"
!endif

!ifndef VI_VERSION
  !define VI_VERSION "0.4.4.0"
!endif

!ifndef STAGE_DIR
  !define STAGE_DIR "..\..\dist\soeasy-v${VERSION}-windows-x64"
!endif

!ifndef OUT_FILE
  !define OUT_FILE "..\..\dist\soeasy-v${VERSION}-windows-x64-setup.exe"
!endif

!define APP_NAME "SoEasy"
!define APP_DISPLAY_NAME "SoEasy Desktop Search Engine"
!define APP_PUBLISHER "SoEasy Team"
!define APP_URL "https://github.com/lilucpp/soeasy"
!define APP_EXE "bin\soeasy-gui.exe"
!define UNINSTALL_REG_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoEasy"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

; Installer / Uninstaller metadata
VIProductVersion "${VI_VERSION}"
VIAddVersionKey "ProductName" "${APP_DISPLAY_NAME}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_DISPLAY_NAME} Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2026 SoEasy Team (Apache-2.0)"

; Modern UI configuration
!define MUI_ABORTWARNING
!define MUI_ICON "${STAGE_DIR}\icons\soeasy.ico"
!define MUI_UNICON "${STAGE_DIR}\icons\soeasy.ico"

; Finish page: offer to launch SoEasy (unchecked by default so daily runs use standard user token)
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

!macro ENSURE_NOT_RUNNING
  ${If} ${FileExists} "$INSTDIR\${APP_EXE}"
    ClearErrors
    FileOpen $0 "$INSTDIR\${APP_EXE}" a
    ${If} ${Errors}
      ${IfNot} ${Silent}
        MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "SoEasy is currently running. Click OK to close it and continue, or Cancel to abort." IDOK +2
        Abort
      ${EndIf}
      nsExec::ExecToLog 'taskkill.exe /F /IM soeasy-gui.exe'
      nsExec::ExecToLog 'taskkill.exe /F /IM soeasy-worker.exe'
      Sleep 500
      ClearErrors
      FileOpen $0 "$INSTDIR\${APP_EXE}" a
      ${If} ${Errors}
        ${IfNot} ${Silent}
          MessageBox MB_OK|MB_ICONSTOP "Unable to close running SoEasy process. Please exit SoEasy from the system tray and try again."
        ${EndIf}
        Abort
      ${EndIf}
      FileClose $0
    ${Else}
      FileClose $0
    ${EndIf}
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "SoEasy requires a 64-bit (x64) version of Windows."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all

  ; Read previous 64-bit install location after SetRegView 64 (avoids 32-bit WOW6432Node lookup)
  ReadRegStr $0 HKLM "${UNINSTALL_REG_KEY}" "InstallLocation"
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "SoEasy Core (Required)" SecCore
  SectionIn RO

  !insertmacro ENSURE_NOT_RUNNING

  SetOutPath "$INSTDIR"
  File /nonfatal "${STAGE_DIR}\README.md"
  File /nonfatal "${STAGE_DIR}\run.bat"

  SetOutPath "$INSTDIR\bin"
  File /r "${STAGE_DIR}\bin\*.*"

  SetOutPath "$INSTDIR\dict"
  File /r "${STAGE_DIR}\dict\*.*"

  SetOutPath "$INSTDIR\models"
  File /r "${STAGE_DIR}\models\*.*"

  SetOutPath "$INSTDIR\icons"
  File /r "${STAGE_DIR}\icons\*.*"

  ; Create uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Start Menu shortcuts (SetOutPath sets working directory for CreateShortcut)
  SetOutPath "$INSTDIR\bin"
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\icons\soeasy.ico" 0
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0

  ; Windows Add/Remove Programs registration
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "DisplayName" "${APP_DISPLAY_NAME}"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "URLInfoAbout" "${APP_URL}"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "DisplayIcon" "$INSTDIR\icons\soeasy.ico"
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTALL_REG_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTALL_REG_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_REG_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINSTALL_REG_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop Shortcut" SecDesktop
  SetOutPath "$INSTDIR\bin"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\icons\soeasy.ico" 0
SectionEnd

LangString DESC_SecCore ${LANG_ENGLISH} "Install SoEasy GUI, worker sandbox, CLI, Jieba dictionary, and OCR models."
LangString DESC_SecCore ${LANG_SIMPCHINESE} "安装 SoEasy 桌面主程序、沙箱解析器、CLI 命令行工具、分词词典及 OCR 模型。"
LangString DESC_SecDesktop ${LANG_ENGLISH} "Create a SoEasy shortcut on the Desktop."
LangString DESC_SecDesktop ${LANG_SIMPCHINESE} "在桌面创建 SoEasy 快捷方式。"

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} $(DESC_SecCore)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_SecDesktop)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  !insertmacro ENSURE_NOT_RUNNING

  ; Remove shortcuts
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"

  ; Remove installed files
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\dict"
  RMDir /r "$INSTDIR\models"
  RMDir /r "$INSTDIR\icons"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\run.bat"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  ; Remove registry entry
  DeleteRegKey HKLM "${UNINSTALL_REG_KEY}"
SectionEnd
