#ifndef __ACPI_H__
#define __ACPI_H__

#include <acpi/tables.h>
#include <boot/stivale2.h>
#include <stddef.h>
#include <vec.h>

typedef vec_t(madt_lapic_t *) vec_madt_lapic;
typedef vec_t(madt_ioapic_t *) vec_madt_ioapic;
typedef vec_t(madt_nmi_t *) vec_madt_nmi;
typedef vec_t(madt_iso_t *) vec_madt_iso;

extern rsdp_t *rsdp;
extern rsdt_t *rsdt;
extern xsdt_t *xsdt;
extern madt_t *madt;
extern mcfg_t *mcfg;
extern sdt_t *facp;

extern vec_madt_lapic madt_lapics;
extern vec_madt_ioapic madt_ioapics;
extern vec_madt_nmi madt_nmis;
extern vec_madt_iso madt_isos;

int init_acpi(struct stivale2_struct_tag_rsdp *rsdp_info);
void *get_table(char *signature, int index);

#endif
