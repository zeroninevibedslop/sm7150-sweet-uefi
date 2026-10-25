/** @file
 *
 *  KeypadDeviceDxe - produtor do gEFIDroidKeypadDeviceProtocol para o sweet.
 *
 *  Le os botoes fisicos (Power + Volume Down) direto do PMIC via SPMI, e expoe
 *  o protocolo EFIDroid de keypad. O KeypadDxe (driver-binding, ja no build)
 *  liga neste protocolo e instala SimpleTextIn, que o menu SimpleInit consome
 *  (uefi_keyboard.c: SCAN_SUSPEND->Enter, SCAN_VOLUME_DOWN->descer).
 *
 *  Acesso SPMI portado do u-boot (drivers/spmi/spmi-msm.c), PMIC Arbiter v5.
 *  Enderecos do sm7150 (sm7150.dtsi, spmi@c440000):
 *    core=0x0C440000 chnls=0x0C600000 obsrvr=0x0E600000 cnfg=0x0C40A000 ee=0
 *
 *  Botoes do sweet (xiaomi-sweet.dtb): PMIC principal pm6150 @ USID 0,
 *  PON (power-on) na base 0x800; RT status em 0x810.
 *    Power (KPDPWR) e Vol-Down (RESIN) lidos do PON_INT_RT_STS.
 *
 *  >>> SE NAO FUNCIONAR DE PRIMEIRA, ajuste o "MAPA DE BOTOES" abaixo. <<<
 **/

#include <PiDxe.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>

#include <Protocol/SimpleTextInEx.h>
#include <Protocol/KeypadDevice.h>

//
// ===================== PMIC ARBITER (SPMI) v5 — sm7150 =====================
//
#define PMIC_ARB_CORE        0x0C440000
#define PMIC_ARB_OBSRVR      0x0E600000
#define PMIC_ARB_CNFG        0x0C40A000   // (nao usado no caminho de leitura)
#define PMIC_ARB_EE          0            // qcom,ee

#define PMIC_ARB_FEATURES        0x0004
#define PMIC_ARB_FEATURES_MASK   0x7FF
#define APID_MAP_OFFSET_V5       0x900
#define SPMI_V5_OBS_CH_OFFSET(c) ((UINTN)(c) * 0x80)

#define SPMI_REG_CMD0        0x00
#define SPMI_REG_CONFIG      0x04
#define SPMI_REG_STATUS      0x08
#define SPMI_REG_RDATA       0x18

#define SPMI_CMD_EXT_REG_READ_LONG  0x01
#define SPMI_STATUS_DONE            0x01

#define SPMI_MAX_CHANNELS_V5  512

//
// ===================== MAPA DE BOTOES DO SWEET (AJUSTAR AQUI) =====================
//
#define PON_USID        0       // pm6150 @ USID 0
#define PON_PID         0x08    // peripheral PON (base 0x800)
#define PON_INT_RT_STS  0x10    // registrador de status em tempo real (0x810)
// Bits do RT_STS. pm6150 e PMIC5 -> normalmente KPDPWR=bit0, RESIN=bit1.
// Se nao pegar, tentar a variante GEN3: KPDPWR=7, RESIN=6.
#define PON_KPDPWR_BIT  0       // Power
#define PON_RESIN_BIT   1       // Volume Down

// Vol-Up: sweet usa "gpios = <&pm6150l_gpios 2 GPIO_ACTIVE_LOW>".
// pm6150l @ USID 4 (pinctrl@c000). GPIO n -> base 0xC000 + (n-1)*0x100.
// GPIO 2 -> 0xC100 -> pid 0xC1. Valor de input no RT_STS (0x10) bit 0.
// GPIO_ACTIVE_LOW => pressionado = nivel 0.
#define VOLUP_USID      4
#define VOLUP_PID       0xC1
#define VOLUP_RT_STS    0x10
#define VOLUP_VAL_MASK  0x01
// ==================================================================================

STATIC INT32  mPonChannel   = -1;   // canal SPMI (APID) do PON
STATIC INT32  mVolUpChannel = -1;   // canal SPMI do GPIO2 do pm6150l (vol-up)
STATIC BOOLEAN mLastPwr   = FALSE;
STATIC BOOLEAN mLastVolDn = FALSE;
STATIC BOOLEAN mLastVolUp = FALSE;

/**
  Acha o canal (APID) do PMIC arbiter para um par (usid, pid) — PMIC Arb v5.
**/
STATIC
INT32
SpmiFindChannel (
  IN UINT8  Usid,
  IN UINT8  Pid
  )
{
  UINT32  MaxCh;
  UINT32  i;

  MaxCh = MmioRead32 (PMIC_ARB_CORE + PMIC_ARB_FEATURES) & PMIC_ARB_FEATURES_MASK;
  if (MaxCh > SPMI_MAX_CHANNELS_V5) {
    MaxCh = SPMI_MAX_CHANNELS_V5;
  }

  for (i = 0; i < MaxCh; i++) {
    UINT32  Periph = MmioRead32 (PMIC_ARB_CORE + APID_MAP_OFFSET_V5 + (i * 4));
    UINT8   Sid    = (UINT8)((Periph >> 16) & 0x0F);
    UINT8   Ppid   = (UINT8)((Periph >> 8) & 0xFF);

    if ((Sid == Usid) && (Ppid == Pid)) {
      return (INT32)i;
    }
  }
  return -1;
}

/**
  Le 1 byte de um registrador do PMIC via o canal observer (read-only).
  Retorna 0..0xFF, ou 0xFFFFFFFF em erro.
**/
STATIC
UINT32
SpmiRead8 (
  IN INT32  Channel,
  IN UINT8  Offset
  )
{
  UINTN   Base;
  UINT32  Cmd;
  UINT32  Status;
  UINTN   Timeout;

  if (Channel < 0) {
    return 0xFFFFFFFF;
  }

  Base = PMIC_ARB_OBSRVR + SPMI_V5_OBS_CH_OFFSET (Channel);
  Cmd  = ((UINT32)SPMI_CMD_EXT_REG_READ_LONG << 27) | ((UINT32)Offset << 4) | 1;

  MmioWrite32 (Base + SPMI_REG_CONFIG, 0);   // desabilita IRQ mode
  MmioWrite32 (Base + SPMI_REG_CMD0, Cmd);   // dispara leitura

  Status  = 0;
  Timeout = 1000000;
  while ((Status == 0) && (Timeout-- > 0)) {
    Status = MmioRead32 (Base + SPMI_REG_STATUS);
  }
  if (Status != SPMI_STATUS_DONE) {
    return 0xFFFFFFFF;
  }

  return MmioRead32 (Base + SPMI_REG_RDATA) & 0xFF;
}

STATIC
VOID
PushKey (
  IN KEYPAD_RETURN_API  *KeypadReturnApi,
  IN UINT16              ScanCode,
  IN CHAR16              UnicodeChar
  )
{
  EFI_KEY_DATA  KeyData;

  KeyData.Key.ScanCode            = ScanCode;
  KeyData.Key.UnicodeChar         = UnicodeChar;
  KeyData.KeyState.KeyShiftState  = 0;
  KeyData.KeyState.KeyToggleState = 0;

  KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &KeyData);
}

//
// ===================== KEYPAD_DEVICE_PROTOCOL =====================
//
STATIC
EFI_STATUS
EFIAPI
KeypadReset (
  IN KEYPAD_DEVICE_PROTOCOL  *This
  )
{
  mLastPwr   = FALSE;
  mLastVolDn = FALSE;
  mLastVolUp = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeypadGetKeys (
  IN KEYPAD_DEVICE_PROTOCOL  *This,
  IN KEYPAD_RETURN_API       *KeypadReturnApi,
  IN UINT64                   Delta
  )
{
  UINT32   Rt;
  BOOLEAN  Pwr;
  BOOLEAN  VolDn;

  Rt = SpmiRead8 (mPonChannel, PON_INT_RT_STS);
  if (Rt == 0xFFFFFFFF) {
    return EFI_DEVICE_ERROR;
  }

  // Check both GEN2 (bits 0,1) and GEN3 (bits 7,6) on PMIC PON
  Pwr   = ((Rt & (1u << PON_KPDPWR_BIT)) != 0) || ((Rt & (1u << 7)) != 0);
  VolDn = ((Rt & (1u << PON_RESIN_BIT))  != 0) || ((Rt & (1u << 6)) != 0);

  // Vol-Up: GPIO2 do pm6150l, ACTIVE_LOW (pressionado = bit 0 em zero).
  {
    UINT32  Gu = SpmiRead8 (mVolUpChannel, VOLUP_RT_STS);
    BOOLEAN VolUp = (Gu != 0xFFFFFFFF) && ((Gu & VOLUP_VAL_MASK) == 0);

    if (VolUp && !mLastVolUp) {
      PushKey (KeypadReturnApi, SCAN_UP, 0);
    }
    mLastVolUp = VolUp;
  }

  // Deteccao de borda (so na transicao solto->apertado) — sem auto-repeat.
  if (Pwr && !mLastPwr) {
    PushKey (KeypadReturnApi, SCAN_SUSPEND, CHAR_CARRIAGE_RETURN);
  }
  if (VolDn && !mLastVolDn) {
    PushKey (KeypadReturnApi, SCAN_DOWN, 0);
  }

  mLastPwr   = Pwr;
  mLastVolDn = VolDn;

  return EFI_SUCCESS;
}

STATIC KEYPAD_DEVICE_PROTOCOL  mKeypadDevice = {
  KeypadReset,
  KeypadGetKeys
};

EFI_STATUS
EFIAPI
KeypadDeviceEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;

  // Acha os canais SPMI (PON e o GPIO do vol-up) uma vez.
  mPonChannel   = SpmiFindChannel (PON_USID, PON_PID);
  mVolUpChannel = SpmiFindChannel (VOLUP_USID, VOLUP_PID);
  DEBUG ((
    DEBUG_ERROR,
    "KeypadDeviceDxe: PON ch=%d (RT=0x%02x), VolUp ch=%d (RT=0x%02x)\n",
    mPonChannel,
    (mPonChannel >= 0) ? SpmiRead8 (mPonChannel, PON_INT_RT_STS) : 0xFF,
    mVolUpChannel,
    (mVolUpChannel >= 0) ? SpmiRead8 (mVolUpChannel, VOLUP_RT_STS) : 0xFF
    ));

  // Instala o protocolo produtor. O KeypadDxe liga nele (via ConnectAll do BDS)
  // e instala SimpleTextIn, que o menu encontra.
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEFIDroidKeypadDeviceProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mKeypadDevice
                  );
  DEBUG ((DEBUG_ERROR, "KeypadDeviceDxe: install keypad protocol = %r\n", Status));

  // Tenta conectar ja (caso o KeypadDxe ja esteja carregado).
  if (!EFI_ERROR (Status)) {
    gBS->ConnectController (Handle, NULL, NULL, TRUE);
  }

  return Status;
}
