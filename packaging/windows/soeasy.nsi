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
!define APP_EXE "soeasy-gui.exe"
!define UNINSTALL_REG_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoEasy"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
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

LangString DESC_SecCore ${LANG_ENGLISH} "Install SoEasy GUI, worker sandbox, CLI, Jieba dictionary, and OCR models."
LangString DESC_SecCore ${LANG_SIMPCHINESE} "安装 SoEasy 桌面主程序、沙箱解析器、CLI 命令行工具、分词词典及 OCR 模型。"
LangString DESC_SecDesktop ${LANG_ENGLISH} "Create a SoEasy shortcut on the Desktop."
LangString DESC_SecDesktop ${LANG_SIMPCHINESE} "在桌面创建 SoEasy 快捷方式。"
LangString MSG_X64Required ${LANG_ENGLISH} "SoEasy requires a 64-bit (x64) version of Windows."
LangString MSG_X64Required ${LANG_SIMPCHINESE} "SoEasy 需要 64 位（x64）Windows 操作系统。"
LangString MSG_Win10Required ${LANG_ENGLISH} "SoEasy requires Windows 10 or later (64-bit)."
LangString MSG_Win10Required ${LANG_SIMPCHINESE} "SoEasy 需要 Windows 10 及以上版本（64 位）操作系统。"
LangString MSG_AppRunning ${LANG_ENGLISH} "SoEasy is currently running. Click OK to close it and continue, or Cancel to abort."
LangString MSG_AppRunning ${LANG_SIMPCHINESE} "SoEasy 正在运行中。点击“确定”将自动关闭程序并继续，点击“取消”中止操作。"
LangString MSG_AppCloseFailed ${LANG_ENGLISH} "Unable to close running SoEasy process. Please exit SoEasy from the system tray and try again."
LangString MSG_AppCloseFailed ${LANG_SIMPCHINESE} "无法关闭运行中的 SoEasy 进程，请先从系统托盘手动退出后再试。"
LangString MSG_RemoveUserData ${LANG_ENGLISH} "Do you also want to remove your personal SoEasy index database, configuration, and logs (%APPDATA%\SoEasy)?$\r$\nChoose No to keep your existing index when upgrading or reinstalling."
LangString MSG_RemoveUserData ${LANG_SIMPCHINESE} "是否同时删除个人索引数据库、配置及日志（%APPDATA%\SoEasy）？$\r$\n选择“否”可在重装或升级时保留现有索引。"

!macro ENSURE_NOT_RUNNING
  ${If} ${FileExists} "$INSTDIR\${APP_EXE}"
    ClearErrors
    FileOpen $0 "$INSTDIR\${APP_EXE}" a
    ${If} ${Errors}
      ${IfNot} ${Silent}
        MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "$(MSG_AppRunning)" IDOK +2
        Abort
      ${EndIf}
      nsExec::ExecToLog 'taskkill.exe /F /IM soeasy-gui.exe'
      nsExec::ExecToLog 'taskkill.exe /F /IM soeasy-worker.exe'
      Sleep 500
      ClearErrors
      FileOpen $0 "$INSTDIR\${APP_EXE}" a
      ${If} ${Errors}
        ${IfNot} ${Silent}
          MessageBox MB_OK|MB_ICONSTOP "$(MSG_AppCloseFailed)"
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
    MessageBox MB_OK|MB_ICONSTOP "$(MSG_X64Required)"
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "$(MSG_Win10Required)"
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
  File /r /x "Uninstall.exe" "${STAGE_DIR}\*"

  ; Create uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Start Menu shortcuts (SetOutPath sets working directory for CreateShortcut)
  SetOutPath "$INSTDIR"
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
  SetOutPath "$INSTDIR"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\icons\soeasy.ico" 0
SectionEnd

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
  Delete "$INSTDIR\*.exe"
  Delete "$INSTDIR\*.dll"
  RMDir /r "$INSTDIR\platforms"
  RMDir /r "$INSTDIR\styles"
  RMDir /r "$INSTDIR\imageformats"
  RMDir /r "$INSTDIR\iconengines"
  RMDir /r "$INSTDIR\bearer"
  RMDir /r "$INSTDIR\translations"
  RMDir /r "$INSTDIR\dict"
  RMDir /r "$INSTDIR\models"
  RMDir /r "$INSTDIR\icons"
  RMDir "$INSTDIR"

  ; Remove registry entry
  DeleteRegKey HKLM "${UNINSTALL_REG_KEY}"

  ; Optional interactive cleanup of per-user index/config data (never deleted in /S silent mode)
  ${IfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 $(MSG_RemoveUserData) IDNO skip_user_data_cleanup
      SetShellVarContext current
      RMDir /r "$APPDATA\SoEasy"
      RMDir /r "$LOCALAPPDATA\soeasy"
      SetShellVarContext all
    skip_user_data_cleanup:
  ${EndIf}
SectionEnd
