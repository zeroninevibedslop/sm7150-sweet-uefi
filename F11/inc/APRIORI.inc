APRIORI DXE {
  #
  # PI DXE Drivers producing Architectural Protocols (EFI Services)
  #
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF ArmPkg/Drivers/ArmGic/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  # WdtKillerDxe RESTAURADO: sweet NAO tem QcomWDogDxe (o servicer que o surya
  # usa), entao PRECISA desabilitar o APSS watchdog (armado pelo XBL) cedo,
  # senao a base nem boota. (Conselho de remover era do surya, que tem servicer.)
  INF F11/Drivers/WdtKillerDxe/WdtKillerDxe.inf
  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf

  #
  # FAT filesystem + GPT/MBR partitioning
  #
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf

  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

  #
  # Ramdisk block device & FAT filesystem (BEFORE BDS!)
  #
  INF F11/Drivers/InitrdBlockDxe/InitrdBlockDxe.inf
  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF FatPkg/EnhancedFatDxe/Fat.inf

  #
  # ================= STRIP-DOWN: BOOT MINIMO =================
  # TODOS os binarios Qualcomm (Smem, HWIO, ChipInfo, Clock, Pmic, Rpmh, Scm,
  # SPMI, Tz, UFS, Pdc, Npa, CmdDb, ShmBridge, DALTLMM, DDRInfo, PlatformInfo,
  # ULog, Buttons) estao DESATIVADOS: removidos daqui e o dxe.inc foi tirado
  # do F11.fdf.
  # Objetivo: confirmar que sem eles o boot chega no BDS/menu, e entao
  # readicionar UM a UM para achar o que re-arma o watchdog.
  # ===========================================================

  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
}
