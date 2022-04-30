#include <acpi/acpi.h>
#include <dev/device.h>
#include <drivers/ahci.h>
#include <drivers/mbr.h>
#include <fs/vfs.h>
#include <klog.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <pci/pci.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vec.h>

#define SATA_SIG_ATA 0x00000101
#define SATA_SIG_ATAPI 0xEB140101
#define SATA_SIG_SEMB 0xC33C0101
#define SATA_SIG_PM 0x96690101

#define AHCI_DEV_NULL 0
#define AHCI_DEV_SATA 1
#define AHCI_DEV_SEMB 2
#define AHCI_DEV_PM 3
#define AHCI_DEV_SATAPI 4

#define HBA_PXCMD_ST 0x0001
#define HBA_PXCMD_FRE 0x0010
#define HBA_PXCMD_FR 0x4000
#define HBA_PXCMD_CR 0x8000

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

#define ATA_CMD_READ_DMA_EX 0x25
#define ATA_CMD_WRITE_DMA_EX 0x35

#define HBA_PXIS_TFES (1 << 30)
#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ 0x08

vec_t(uint64_t) abars = {};

// Thanks to OSDEV.org for these 5 snippets
static inline uint8_t ahci_check_type(hba_port_t *port) {
  uint32_t ssts = port->ssts;
  uint8_t ipm = (ssts >> 8) & 0x0F;
  uint8_t det = ssts & 0x0F;

  if (det != HBA_PORT_DET_PRESENT)
    return AHCI_DEV_NULL;
  if (ipm != HBA_PORT_IPM_ACTIVE)
    return AHCI_DEV_NULL;

  switch (port->sig) {
    case SATA_SIG_ATAPI:
      return AHCI_DEV_SATAPI;
    case SATA_SIG_SEMB:
      return AHCI_DEV_SEMB;
    case SATA_SIG_PM:
      return AHCI_DEV_PM;
    default:
      return AHCI_DEV_SATA;
  }
}

void ahci_start_cmd_engine(hba_port_t *port) {
  while (port->cmd & HBA_PXCMD_CR)
    ;

  port->cmd |= HBA_PXCMD_FRE;
  port->cmd |= HBA_PXCMD_ST;
}

void ahci_stop_cmd_engine(hba_port_t *port) {
  port->cmd &= ~HBA_PXCMD_ST;
  port->cmd &= ~HBA_PXCMD_FRE;

  while (1) {
    if (port->cmd & HBA_PXCMD_FR)
      continue;
    if (port->cmd & HBA_PXCMD_CR)
      continue;
    break;
  }
}

void ahci_port_init(hba_port_t *port) {
  ahci_stop_cmd_engine(port);

  port->clb = (uint32_t)(uint64_t)pcalloc(1);
  port->clbu = 0;

  port->fb = (uint32_t)(uint64_t)pcalloc(1);
  port->fbu = 0;

  hba_cmd_header_t *cmd_header = (hba_cmd_header_t *)(uint64_t)(port->clb);

  for (int i = 0; i < 32; i++) {
    cmd_header[i].prdtl = 8;
    cmd_header[i].ctba = (uint32_t)(uint64_t)pcalloc(1);
    cmd_header[i].ctbau = 0;
  }

  ahci_start_cmd_engine(port);
}

int8_t ahci_find_cmdslot(hba_port_t *port) {
  uint32_t slots = (port->sact | port->ci);

  for (size_t i = 0; i < 32; i++) {
    if (!(slots & 1))
      return i;
    slots >>= 1;
  }

  return -1;
}
// End snippets

ssize_t sata_read(device_t *dev, size_t start, size_t count, uint8_t *buf) {
  uint32_t count32 = (uint32_t)count;
  uint64_t start64 = (uint64_t)start;

  hba_port_t *port = ((ahci_private_data_t *)dev->private_data)->port;

  if (((ahci_private_data_t *)dev->private_data)->part)
    start64 += ((ahci_private_data_t *)dev->private_data)->part->sector_start;

  uint32_t startl = (uint32_t)start64;
  uint32_t starth = (uint32_t)(start64 >> 32);

  port->is = (uint32_t)-1;

  int8_t slot = ahci_find_cmdslot(port);
  if (slot == -1)
    return 0;

  hba_cmd_header_t *cmd_header =
    (hba_cmd_header_t *)((uintptr_t)(port->clb + PHYS_MEM_OFFSET));
  cmd_header += slot;
  cmd_header->cfl = sizeof(fis_reg_host_to_device_t) / sizeof(uint32_t);
  cmd_header->w = 0;
  cmd_header->prdtl = (uint16_t)((count32 - 1) >> 4) + 1;

  hba_cmd_tbl_t *cmd_table =
    (hba_cmd_tbl_t *)((uintptr_t)cmd_header->ctba + PHYS_MEM_OFFSET);
  memset(cmd_table, 0,
         sizeof(hba_cmd_tbl_t) +
           (cmd_header->prdtl - 1) * sizeof(hba_prdt_entry_t));

  size_t i;
  for (i = 0; i < (size_t)cmd_header->prdtl - 1; i++) {
    cmd_table->prdt_entry[i].dba = (uint32_t)(uintptr_t)(buf - PHYS_MEM_OFFSET);
    cmd_table->prdt_entry[i].dbau = 0;
    cmd_table->prdt_entry[i].dbc = 8 * 1024 - 1; // 8KiB - 1
    cmd_table->prdt_entry[i].i = 1;

    buf += 8 * 1024;
    count32 -= 16;
  }

  cmd_table->prdt_entry[i].dba =
    (uint32_t)(uintptr_t)((uint64_t)buf - PHYS_MEM_OFFSET);
  cmd_table->prdt_entry[i].dbc = (count32 << 9) - 1;
  cmd_table->prdt_entry[i].i = 1;

  fis_reg_host_to_device_t *cmd_fis =
    (fis_reg_host_to_device_t *)(&cmd_table->cfis);

  cmd_fis->fis_type = FIS_TYPE_REG_HOST_TO_DEVICE;
  cmd_fis->c = 1;
  cmd_fis->command = ATA_CMD_READ_DMA_EX;

  cmd_fis->lba0 = (uint8_t)startl;
  cmd_fis->lba1 = (uint8_t)(startl >> 8);
  cmd_fis->lba2 = (uint8_t)(startl >> 16);
  cmd_fis->device = 1 << 6;

  cmd_fis->lba3 = (uint8_t)(startl >> 24);
  cmd_fis->lba4 = (uint8_t)(starth);
  cmd_fis->lba5 = (uint8_t)(starth >> 8);

  cmd_fis->countl = (count32 & 0xFF);
  cmd_fis->counth = (count32 >> 8);

  for (uint32_t spin = 0; spin < 1000000; spin++) {
    if (!(port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)))
      break;
  }
  if ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)))
    return 0;

  port->ci = (1 << slot);

  while (1) {
    if (!(port->ci & (1 << slot)))
      break;
    if (port->is & HBA_PXIS_TFES)
      return 0;
  }

  if (port->is & HBA_PXIS_TFES)
    return 0;

  return count * 512;
}

ssize_t sata_write(device_t *dev, size_t start, size_t count, uint8_t *buf) {
  uint32_t count32 = (uint32_t)count;
  uint64_t start64 = (uint64_t)start;

  hba_port_t *port = ((ahci_private_data_t *)dev->private_data)->port;

  if (((ahci_private_data_t *)dev->private_data)->part)
    start64 += ((ahci_private_data_t *)dev->private_data)->part->sector_start;

  uint32_t startl = (uint32_t)start64;
  uint32_t starth = (uint32_t)(start64 >> 32);

  port->is = (uint32_t)-1;

  int8_t slot = ahci_find_cmdslot(port);
  if (slot == -1)
    return 0;

  hba_cmd_header_t *cmd_header =
    (hba_cmd_header_t *)((uintptr_t)(port->clb + PHYS_MEM_OFFSET));
  cmd_header += slot;
  cmd_header->cfl = sizeof(fis_reg_host_to_device_t) / sizeof(uint32_t);
  cmd_header->w = 1;
  cmd_header->c = 1;
  cmd_header->p = 1;
  cmd_header->prdtl = (uint16_t)((count32 - 1) >> 4) + 1;

  hba_cmd_tbl_t *cmd_table =
    (hba_cmd_tbl_t *)((uintptr_t)cmd_header->ctba + PHYS_MEM_OFFSET);
  memset(cmd_table, 0,
         sizeof(hba_cmd_tbl_t) +
           (cmd_header->prdtl - 1) * sizeof(hba_prdt_entry_t));

  size_t i;
  for (i = 0; i < (size_t)cmd_header->prdtl - 1; i++) {
    cmd_table->prdt_entry[i].dba = (uint32_t)(uintptr_t)(buf - PHYS_MEM_OFFSET);
    cmd_table->prdt_entry[i].dbau = 0;
    cmd_table->prdt_entry[i].dbc = 8 * 1024 - 1; // 8KiB - 1
    cmd_table->prdt_entry[i].i = 1;

    buf += 8 * 1024;
    count32 -= 16;
  }

  cmd_table->prdt_entry[i].dba =
    (uint32_t)(uintptr_t)((uint64_t)buf - PHYS_MEM_OFFSET);
  cmd_table->prdt_entry[i].dbc = (count32 << 9) - 1;
  cmd_table->prdt_entry[i].i = 1;

  fis_reg_host_to_device_t *cmd_fis =
    (fis_reg_host_to_device_t *)(&cmd_table->cfis);

  cmd_fis->fis_type = FIS_TYPE_REG_HOST_TO_DEVICE;
  cmd_fis->c = 1;
  cmd_fis->command = ATA_CMD_WRITE_DMA_EX;

  cmd_fis->lba0 = (uint8_t)startl;
  cmd_fis->lba1 = (uint8_t)(startl >> 8);
  cmd_fis->lba2 = (uint8_t)(startl >> 16);
  cmd_fis->device = 1 << 6;

  cmd_fis->lba3 = (uint8_t)(startl >> 24);
  cmd_fis->lba4 = (uint8_t)(starth);
  cmd_fis->lba5 = (uint8_t)(starth >> 8);

  cmd_fis->countl = (count32 & 0xFF);
  cmd_fis->counth = (count32 >> 8);

  for (uint32_t spin = 0; spin < 1000000; spin++) {
    if (!(port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)))
      break;
  }
  if ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)))
    return 0;

  port->ci = (1 << slot);

  while (1) {
    if (!(port->ci & (1 << slot)))
      break;
    if (port->is & HBA_PXIS_TFES)
      return 0;
  }

  if (port->is & HBA_PXIS_TFES)
    return 0;

  return count * 512;
}

void ahci_init_abars() {
  for (size_t j = 0; j < (size_t)abars.length; j++) {
    hba_mem_t *abar = (hba_mem_t *)abars.data[j];

    for (uint32_t i = 0, pi = abar->pi; i < 32; i++, pi >>= 1) {
      if (pi & 1)
        if (ahci_check_type(&abar->ports[i]) == AHCI_DEV_SATA) {
          ahci_port_init(&abar->ports[i]);

          for (size_t p = 0; p < 4; p++) {
            device_t *dev = kmalloc(sizeof(device_t));
            *dev = (device_t){
              .private_data = kmalloc(sizeof(ahci_private_data_t)),
              .name = "SATA",
              .type = S_IFBLK,
              .block_count = 0, // TODO: figure out block count
              .block_size = 512,
              .read = sata_read,
              .write = sata_write,
            };

            *((ahci_private_data_t *)dev->private_data) = (ahci_private_data_t){
              .port = &abar->ports[i],
              .part = NULL,
            };

            partition_layout_t *part = probe_mbr(dev, p);

            if (part) {
              ((ahci_private_data_t *)dev->private_data)->part = part;
              device_add(dev);
            } else {
              kfree(dev->private_data);
              kfree(dev);
            }
          }

          device_t *main_dev = kmalloc(sizeof(device_t));
          *main_dev = (device_t){
            .private_data = kmalloc(sizeof(ahci_private_data_t)),
            .name = "SATA",
            .type = S_IFBLK,
            .block_count = 0, // TODO: figure out block count
            .block_size = 512,
            .read = sata_read,
            .write = sata_write,
          };

          *((ahci_private_data_t *)main_dev->private_data) =
            (ahci_private_data_t){
              .port = &abar->ports[i],
              .part = NULL,
            };

          device_add(main_dev);
        }
    }
  }
}

int init_sata() {
  abars.data = kcalloc(sizeof(uint64_t));

  for (size_t i = 0; i < (size_t)pci_devices.length; i++)
    if (pci_devices.data[i]->header.class == 1 &&
        pci_devices.data[i]->header.subclass == 6)
      vec_push(&abars,
               pci_get_bar(pci_devices.data[i], 5).base + PHYS_MEM_OFFSET);

  if (!abars.length)
    return 1;

  ahci_init_abars();

  return 0;
}
