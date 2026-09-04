; [Input] Tauri NSIS install/uninstall lifecycle and the owned-runtime helper.
; [Output] Stops exact-path Pet Manager desktop/Node processes before file changes.
; [Pos] Windows-only Tauri installer hooks; macOS packaging never reads this file.

!include "LogicLib.nsh"
!define PET_MANAGER_INSTALLER_HOOK_DIR "${__FILEDIR__}"

!macro PET_MANAGER_STOP_OWNED_RUNTIMES
  InitPluginsDir
  File /oname=$PLUGINSDIR\pet-manager-stop-owned-runtimes.ps1 "${PET_MANAGER_INSTALLER_HOOK_DIR}\stop-owned-runtimes.ps1"
  nsExec::ExecToStack '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\pet-manager-stop-owned-runtimes.ps1" -InstallDir "$INSTDIR"'
  Pop $0
  Pop $1
  ${If} $0 != 0
    MessageBox MB_ICONSTOP|MB_OK "Pet Manager background services could not be stopped.$\r$\nPet Manager 后台服务仍在运行，请关闭应用后重试。$\r$\n$1"
    Abort
  ${EndIf}
!macroend

!macro NSIS_HOOK_PREINSTALL
  !insertmacro PET_MANAGER_STOP_OWNED_RUNTIMES
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  !insertmacro PET_MANAGER_STOP_OWNED_RUNTIMES
!macroend
