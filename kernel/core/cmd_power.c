/* EMBODIOS Power Management
 *
 * Bare-metal x86_64 power control:
 *   shutdown / poweroff   ACPI S5 poweroff (PM1a_CNT: SLP_TYP|SLP_EN)
 *   reboot                PCI reset (0xCF9) -> 8042 pulse -> triple fault
 *   power                 status: uptime, idle (hlt) stats, IRQ state
 *
 * The PM1a control block is located by probing the PCI ACPI function:
 *   - PIIX4 ACPI  (i440fx, QEMU 'pc'):  vendor 8086 device 7113, PMBASE @ 0x40
 *   - ICH9 LPC    (Q35):                vendor 8086 device 2918, PMBASE @ 0x40
 * PM1a_CNT_BLK = PMBASE + 0x04.  If no ACPI device is found we fall back to
 * the QEMU default I/O base 0x600 (PM1a_CNT at 0x604).
 *
 * The in-kernel test framework keeps using isa-debug-exit (port 0x501) for
 * its own shutdown path - this file does not interfere with it.
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#include <embodios/cmd_power.h>
#include <embodios/console.h>
#include <embodios/interrupt.h>
#include <embodios/kernel.h>
#include <embodios/pci.h>
#include <embodios/ui.h>

/* ========================================================================
 * Local port I/O (self-contained; pci.c's outb/outw are static inline)
 * ======================================================================== */

static inline void pm_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t pm_inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void pm_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void pm_cli(void) { __asm__ volatile("cli"); }
static inline void pm_hlt(void) { __asm__ volatile("hlt"); }

/* Simple busy-wait (~ms scale) to let a power/reset request take effect
 * before trying the next fallback. */
static void pm_delay(void)
{
    for (volatile int i = 0; i < 1000000; i++)
        __asm__ volatile("pause");
}

/* ========================================================================
 * ACPI shutdown (S5)
 * ======================================================================== */

/* PCI config-space constants for the southbridge ACPI function */
#define ACPI_VENDOR_INTEL   0x8086
#define ACPI_DEV_PIIX4      0x7113  /* PIIX4 ACPI (i440fx, dev 1 fn 3) */
#define ACPI_DEV_ICH9       0x2918  /* ICH9 LPC  (Q35, dev 1f fn 0)    */
#define ACPI_PMBASE_REG     0x40
#define ACPI_PMBASE_MASK    0xFFC0  /* bits 15:6 = I/O base, bit 0 = en */
#define ACPI_PM1A_CNT_OFF   0x04

#define ACPI_SLP_EN         (1u << 13)
#define ACPI_SLP_TYP_SHIFT  10

/* QEMU default when PMBASE reads back as 0 */
#define QEMU_DEFAULT_PMBASE 0x600

struct acpi_chip {
    uint16_t device_id;
    uint8_t  slp_typ_s5;    /* SLP_TYP value that means S5 on this chip */
    const char* name;
};

static const struct acpi_chip acpi_chips[] = {
    { ACPI_DEV_PIIX4, 0, "PIIX4" },  /* QEMU i440fx: S5 = SLP_TYP 0 */
    { ACPI_DEV_ICH9,  5, "ICH9"  },  /* Q35:         S5 = SLP_TYP 5 */
};

/* Locate PM1a_CNT_BLK I/O port. Returns port, and the SLP_TYP for S5
 * via *slp_typ. Falls back to QEMU defaults when PCI probing fails. */
static uint16_t acpi_pm1a_cnt(uint8_t* slp_typ, const char** chip_name)
{
    /* Scan bus 0 (southbridges always live there) for a known ACPI chip */
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            pci_addr_t addr = { 0, dev, fn };
            uint32_t id = pci_config_read32(addr, 0x00);
            if ((uint16_t)id != ACPI_VENDOR_INTEL)
                continue;
            uint16_t device = (uint16_t)(id >> 16);
            for (size_t i = 0; i < sizeof(acpi_chips) / sizeof(acpi_chips[0]); i++) {
                if (device != acpi_chips[i].device_id)
                    continue;
                uint32_t pmbase = pci_config_read32(addr, ACPI_PMBASE_REG);
                uint16_t base = (uint16_t)(pmbase & ACPI_PMBASE_MASK);
                if (base == 0)
                    base = QEMU_DEFAULT_PMBASE;
                *slp_typ = acpi_chips[i].slp_typ_s5;
                *chip_name = acpi_chips[i].name;
                return (uint16_t)(base + ACPI_PM1A_CNT_OFF);
            }
        }
    }

    /* Unknown chipset: QEMU default PM base, PIIX4-style SLP_TYP */
    *slp_typ = 0;
    *chip_name = "generic";
    return QEMU_DEFAULT_PMBASE + ACPI_PM1A_CNT_OFF;
}

void power_shutdown(void)
{
    uint8_t slp_typ;
    const char* chip;
    uint16_t cnt = acpi_pm1a_cnt(&slp_typ, &chip);

    console_printf("Powering off (ACPI S5: %s, PM1a_CNT=0x%x)...\n", chip, cnt);
    console_flush();
    pm_cli();

    /* Primary: detected SLP_TYP for this chipset */
    pm_outw(cnt, (uint16_t)((slp_typ << ACPI_SLP_TYP_SHIFT) | ACPI_SLP_EN));
    pm_delay();

    /* Fallback: the other common S5 encoding (covers mis-detected chips) */
    pm_outw(cnt, (uint16_t)(((slp_typ == 0 ? 5 : 0) << ACPI_SLP_TYP_SHIFT) |
                            ACPI_SLP_EN));
    pm_delay();

    /* QEMU 1.x / Bochs legacy port, harmless on modern QEMU */
    pm_outw(0xB004, (uint16_t)(0 | ACPI_SLP_EN));
    pm_delay();

    console_printf("ACPI poweroff failed (no S5 response). System halted.\n");
    for (;;)
        pm_hlt();
}

/* ========================================================================
 * Reboot
 * ======================================================================== */

void power_reboot(void)
{
    console_printf("Rebooting...\n");
    console_flush();
    pm_cli();

    /* 1) PCI reset control register (port 0xCF9):
     *    bit 1 (0x02) = hard reset, bit 2 (0x04) = full reset w/ power cycle */
    pm_outb(0xCF9, 0x02);   /* request hard reset */
    pm_delay();
    pm_outb(0xCF9, 0x06);   /* assert */
    pm_delay();

    /* 2) 8042 keyboard controller: pulse CPU reset line */
    for (int i = 0; i < 100000; i++) {
        if (!(pm_inb(0x64) & 0x02))
            break;          /* wait for input buffer empty */
    }
    pm_outb(0x64, 0xFE);
    pm_delay();

    /* 3) Triple fault: empty IDT + software interrupt */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("lidt %0" : : "m"(null_idt));
    __asm__ volatile("int $3");

    /* 4) Should never get here */
    for (;;)
        pm_hlt();
}

/* ========================================================================
 * 'power' status command
 * ======================================================================== */

static void power_status(void)
{
    uint64_t ticks = timer_get_ticks();
    uint32_t freq = timer_get_frequency();
    uint64_t secs = freq ? (ticks / freq) : 0;
    uint64_t idle = kernel_idle_hlt_count();

    /* Each hlt sleeps until the next PIT tick, so hlt_count/ticks is a
     * rough lower bound for the fraction of time spent idle. */
    uint64_t idle_pct = ticks ? (idle * 100) / ticks : 0;
    if (idle_pct > 100)
        idle_pct = 100;

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));

    console_printf("\nPower status\n");
    console_printf("  Uptime:       %llu s (%llu ticks @ %u Hz)\n",
                   (unsigned long long)secs,
                   (unsigned long long)ticks, freq);
    console_printf("  Idle (hlt):   %llu loops, ~%llu%% of ticks\n",
                   (unsigned long long)idle,
                   (unsigned long long)idle_pct);
    console_printf("  Interrupts:   %s (IF=%d)\n",
                   kernel_interrupts_enabled() ? "enabled" : "disabled (polling)",
                   (int)((rflags >> 9) & 1));
    console_printf("  Sleep state:  hlt @ IRQ0 %u Hz quantum\n\n", freq);
}

/* ========================================================================
 * Shell dispatch
 * ======================================================================== */

int cmd_power_dispatch(const char* command)
{
    if (strcmp(command, "shutdown") == 0 || strcmp(command, "poweroff") == 0 ||
        strcmp(command, "halt") == 0) {
        power_shutdown();
        return 1;
    }
    if (strcmp(command, "reboot") == 0 || strcmp(command, "reset") == 0) {
        power_reboot();
        return 1;
    }
    if (strcmp(command, "power") == 0) {
        power_status();
        return 1;
    }
    return 0;
}
