!include "MUI2.nsh"
!include "x64.nsh"

!ifndef NEXA_VERSION
  !define NEXA_VERSION "1.0.0"
!endif
!ifndef DIST_DIR
  !define DIST_DIR "dist"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "NexaAssistantSetup.exe"
!endif

Name "Nexa Assistant"
OutFile "${OUT_FILE}"
Unicode True
InstallDir "$PROGRAMFILES64\Nexa Assistant"
InstallDirRegKey HKLM "Software\Nexa\NexaAssistant" "InstallDir"
RequestExecutionLevel admin

VIProductVersion "${NEXA_VERSION}.0"
VIAddVersionKey "ProductName" "Nexa Assistant"
VIAddVersionKey "CompanyName" "Nexa"
VIAddVersionKey "FileDescription" "Nexa Assistant Installer"
VIAddVersionKey "FileVersion" "${NEXA_VERSION}.0"
VIAddVersionKey "ProductVersion" "${NEXA_VERSION}.0"
VIAddVersionKey "LegalCopyright" "Nexa"

!define MUI_ICON "nexa-assistant.ico"
!define MUI_UNICON "nexa-assistant.ico"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetRegView 64
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "Nexa Assistant requires 64-bit Windows (x64)."
    Abort
  ${EndIf}
FunctionEnd

Section "Install"
  DetailPrint "Closing any running Nexa Assistant instance..."
  nsExec::Exec 'taskkill /IM nexa-assistant.exe /F'
  Sleep 500
  SetOutPath "$INSTDIR"
  SetRegView 64
  File /r "${DIST_DIR}\*"
  WriteRegStr HKLM "Software\Nexa\NexaAssistant" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "DisplayName" "Nexa Assistant"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "Publisher" "Nexa"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "DisplayVersion" "${NEXA_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "QuietUninstallString" "$\"$INSTDIR\uninstall.exe$\" /S"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "DisplayIcon" "$INSTDIR\nexa-assistant.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Nexa Assistant"
  CreateShortCut "$SMPROGRAMS\Nexa Assistant\Nexa Assistant.lnk" "$INSTDIR\nexa-assistant.exe"
  CreateShortCut "$SMPROGRAMS\Nexa Assistant\Uninstall.lnk" "$INSTDIR\uninstall.exe"
  CreateShortCut "$DESKTOP\Nexa Assistant.lnk" "$INSTDIR\nexa-assistant.exe"
SectionEnd

Section "Uninstall"
  DetailPrint "Closing any running Nexa Assistant instance..."
  nsExec::Exec 'taskkill /IM nexa-assistant.exe /F'
  Sleep 500
  SetRegView 64
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NexaAssistant"
  DeleteRegKey HKLM "Software\Nexa\NexaAssistant"
  RMDir /r "$INSTDIR"
  Delete "$DESKTOP\Nexa Assistant.lnk"
  RMDir /r "$SMPROGRAMS\Nexa Assistant"
SectionEnd
