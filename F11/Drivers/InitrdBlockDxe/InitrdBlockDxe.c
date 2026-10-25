/** @file
  InitrdBlockDxe - Creates an EFI_BLOCK_IO_PROTOCOL device for the
  SWEETESP ramdisk loaded by ABL into memory.

  Copyright (c) 2026 UniXNU Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <libfdt.h>

#define INITRD_BLOCK_IO_SIGNATURE SIGNATURE_32 ('I', 'R', 'B', 'D')

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} INITRD_DEVICE_PATH;
#pragma pack()

STATIC CONST INITRD_DEVICE_PATH mDevicePathTemplate = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0xb1ab4794, 0x19cd, 0x42ab, { 0x8c, 0x08, 0x16, 0x6f, 0x21, 0x0e, 0x99, 0x02 } }
  },
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

typedef struct {
  UINT32                   Signature;
  EFI_HANDLE               Handle;
  EFI_PHYSICAL_ADDRESS     BaseAddress;
  UINT64                   Size;
  EFI_BLOCK_IO_PROTOCOL    BlockIo;
  EFI_BLOCK_IO_MEDIA       Media;
  INITRD_DEVICE_PATH       DevicePath;
} INITRD_BLOCK_IO_DEV;

STATIC
EFI_STATUS
EFIAPI
InitrdBlockIoReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
InitrdBlockIoReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  INITRD_BLOCK_IO_DEV *Dev;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  Dev = CR (This, INITRD_BLOCK_IO_DEV, BlockIo, INITRD_BLOCK_IO_SIGNATURE);

  if (MediaId != Dev->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if ((BufferSize % Dev->Media.BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Lba + (BufferSize / Dev->Media.BlockSize) > Dev->Media.LastBlock + 1) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (
    Buffer,
    (VOID *)(UINTN)(Dev->BaseAddress + MultU64x32 (Lba, Dev->Media.BlockSize)),
    BufferSize
    );

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
InitrdBlockIoWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  INITRD_BLOCK_IO_DEV *Dev;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  Dev = CR (This, INITRD_BLOCK_IO_DEV, BlockIo, INITRD_BLOCK_IO_SIGNATURE);

  if (MediaId != Dev->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if ((BufferSize % Dev->Media.BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Lba + (BufferSize / Dev->Media.BlockSize) > Dev->Media.LastBlock + 1) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (
    (VOID *)(UINTN)(Dev->BaseAddress + MultU64x32 (Lba, Dev->Media.BlockSize)),
    Buffer,
    BufferSize
    );

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
InitrdBlockIoFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
IsSweetEsp (
  IN  EFI_PHYSICAL_ADDRESS Base,
  OUT UINT64               *SizeOut
  )
{
  UINT8  *Sector;
  UINT16 Sig;
  UINT16 BytesPerSector;
  UINT32 TotalSectors;

  // Invalidate cache line to ensure we read physical DDR contents
  InvalidateDataCacheRange ((VOID *)(UINTN)Base, 512);

  Sector = (UINT8 *)(UINTN)Base;
  Sig = Sector[510] | ((UINT16)Sector[511] << 8);

  BytesPerSector = Sector[11] | ((UINT16)Sector[12] << 8);

  TotalSectors = Sector[32] |
                 ((UINT32)Sector[33] << 8) |
                 ((UINT32)Sector[34] << 16) |
                 ((UINT32)Sector[35] << 24);

  if (TotalSectors == 0) {
    TotalSectors = Sector[19] | ((UINT32)Sector[20] << 8);
  }

  if (Sig == 0xAA55) {
    if (CompareMem (&Sector[71], "SWEETESP", 8) == 0 ||
        CompareMem (&Sector[43], "SWEETESP", 8) == 0) {
      if (TotalSectors > 1000 && BytesPerSector == 512) {
        *SizeOut = (UINT64)TotalSectors * (UINT64)BytesPerSector;
      } else {
        *SizeOut = 26ULL * 1024ULL * 1024ULL; // Default to 26 MiB
      }
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
EFI_PHYSICAL_ADDRESS
LocateSweetEsp (
  OUT UINT64 *SizeOut
  )
{
  EFI_PHYSICAL_ADDRESS Addr;
  EFI_PHYSICAL_ADDRESS FdtPtr;
  VOID                 *Fdt;
  INTN                 Node;
  INT32                Len;
  CONST VOID           *Prop;
  UINT64               InitrdStart;
  UINT8                *DbgSec;

  // 1. Diagnostic: Check what is at 0x81000000
  DbgSec = (UINT8 *)(UINTN)0x81000000ULL;
  InvalidateDataCacheRange ((VOID *)DbgSec, 512);
  DEBUG ((DEBUG_ERROR, "\n[InitrdBlockDxe] Testing 0x81000000: Sig=0x%02x%02x [0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
          DbgSec[510], DbgSec[511],
          DbgSec[0], DbgSec[1], DbgSec[2], DbgSec[3],
          DbgSec[4], DbgSec[5], DbgSec[6], DbgSec[7]));
  DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] Testing 0x81000000: Label43=[%c%c%c%c%c%c%c%c] Label71=[%c%c%c%c%c%c%c%c]\n",
          DbgSec[43], DbgSec[44], DbgSec[45], DbgSec[46],
          DbgSec[47], DbgSec[48], DbgSec[49], DbgSec[50],
          DbgSec[71], DbgSec[72], DbgSec[73], DbgSec[74],
          DbgSec[75], DbgSec[76], DbgSec[77], DbgSec[78]));

  if (IsSweetEsp (0x81000000ULL, SizeOut)) {
    DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] SUCCESS: Found SWEETESP at 0x81000000 (size %lu MiB)\n",
            (*SizeOut) / (1024 * 1024)));
    return 0x81000000ULL;
  }

  // 2. Check FDT passed by ABL (stored at 0x83300000 by F11Helper.S)
  FdtPtr = *(EFI_PHYSICAL_ADDRESS *)(UINTN)0x83300000ULL;
  DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] FDT ptr stored at 0x83300000 = 0x%lx\n", FdtPtr));
  if ((FdtPtr >= 0x80000000ULL) && (FdtPtr < 0x180000000ULL)) {
    Fdt = (VOID *)(UINTN)FdtPtr;
    if (fdt_check_header (Fdt) == 0) {
      DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] Valid FDT header detected at 0x%lx\n", FdtPtr));
      Node = fdt_path_offset (Fdt, "/chosen");
      if (Node >= 0) {
        Prop = fdt_getprop (Fdt, Node, "linux,initrd-start", &Len);
        if (Prop != NULL) {
          InitrdStart = (Len == 8) ? fdt64_to_cpu (*(UINT64 *)Prop) : fdt32_to_cpu (*(UINT32 *)Prop);
          DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] FDT /chosen linux,initrd-start = 0x%lx\n", InitrdStart));
          if (IsSweetEsp ((EFI_PHYSICAL_ADDRESS)InitrdStart, SizeOut)) {
            DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] SUCCESS: Found SWEETESP at 0x%lx from FDT (size %lu MiB)\n",
                    InitrdStart, (*SizeOut) / (1024 * 1024)));
            return (EFI_PHYSICAL_ADDRESS)InitrdStart;
          }
        }
      }
    }
  }

  // 3. Scan safe low memory (0x80000000 - 0x83000000) in 64 KiB steps
  for (Addr = 0x80000000ULL; Addr < 0x83000000ULL; Addr += 0x10000ULL) {
    if (IsSweetEsp (Addr, SizeOut)) {
      DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] SUCCESS: Found SWEETESP in low DDR at 0x%lx (size %lu MiB)\n",
              Addr, (*SizeOut) / (1024 * 1024)));
      return Addr;
    }
  }

  // 4. Scan conventional memory (0xA0000000 - 0xD0000000) in 1 MiB steps
  for (Addr = 0xA0000000ULL; Addr < 0xD0000000ULL; Addr += 0x100000ULL) {
    if (IsSweetEsp (Addr, SizeOut)) {
      DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] SUCCESS: Found SWEETESP in conv DDR at 0x%lx (size %lu MiB)\n",
              Addr, (*SizeOut) / (1024 * 1024)));
      return Addr;
    }
  }

  return 0;
}

EFI_STATUS
EFIAPI
InitrdBlockDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS RamdiskBase;
  UINT64               RamdiskSize;
  INITRD_BLOCK_IO_DEV  *Dev;

  RamdiskBase = 0;
  RamdiskSize = 0;

  RamdiskBase = LocateSweetEsp (&RamdiskSize);
  if (RamdiskBase == 0) {
    DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] ERROR: SWEETESP volume not found anywhere in RAM!\n"));
    return EFI_NOT_FOUND;
  }

  Dev = AllocateZeroPool (sizeof (INITRD_BLOCK_IO_DEV));
  if (Dev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Dev->Signature   = INITRD_BLOCK_IO_SIGNATURE;
  Dev->Handle      = NULL;
  Dev->BaseAddress = RamdiskBase;
  Dev->Size        = RamdiskSize;

  Dev->BlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Dev->BlockIo.Media       = &Dev->Media;
  Dev->BlockIo.Reset       = InitrdBlockIoReset;
  Dev->BlockIo.ReadBlocks  = InitrdBlockIoReadBlocks;
  Dev->BlockIo.WriteBlocks = InitrdBlockIoWriteBlocks;
  Dev->BlockIo.FlushBlocks = InitrdBlockIoFlushBlocks;

  Dev->Media.MediaId           = 1;
  Dev->Media.RemovableMedia    = FALSE;
  Dev->Media.MediaPresent      = TRUE;
  Dev->Media.LogicalPartition  = FALSE;
  Dev->Media.ReadOnly          = FALSE;
  Dev->Media.WriteCaching      = FALSE;
  Dev->Media.BlockSize         = 512;
  Dev->Media.IoAlign           = 0;
  Dev->Media.LastBlock         = (RamdiskSize / 512) - 1;
  Dev->Media.LowestAlignedLba  = 0;
  Dev->Media.LogicalBlocksPerPhysicalBlock = 1;

  CopyMem (&Dev->DevicePath, &mDevicePathTemplate, sizeof (INITRD_DEVICE_PATH));

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Dev->Handle,
                  &gEfiBlockIoProtocolGuid, &Dev->BlockIo,
                  &gEfiDevicePathProtocolGuid, &Dev->DevicePath,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] ERROR: InstallMultipleProtocolInterfaces failed: %r\n", Status));
    FreePool (Dev);
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "[InitrdBlockDxe] BlockIo installed on handle %p (0x%lx, %lu sectors)\n",
          Dev->Handle, Dev->BaseAddress, Dev->Media.LastBlock + 1));

  // Connect controllers to bind DiskIo and SimpleFileSystem immediately
  gBS->ConnectController (Dev->Handle, NULL, NULL, TRUE);

  return EFI_SUCCESS;
}
