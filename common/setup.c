/*
 * Copyright © 2020 Amazon.com, Inc. or its affiliates.
 * All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <acpi_ktf.h>
#include <apic.h>
#include <cmdline.h>
#include <console.h>
#include <cpuid.h>
#include <drivers/keyboard.h>
#include <ioapic.h>
#include <ktf.h>
#include <lib.h>
#include <multiboot.h>
#include <page.h>
#include <pagetable.h>
#include <pci.h>
#include <percpu.h>
#include <real_mode.h>
#include <sched.h>
#include <segment.h>
#include <setup.h>
#include <string.h>
#include <traps.h>

#include <mm/pmm.h>
#include <mm/regions.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <smp/mptables.h>
#include <smp/smp.h>

#include <drivers/fb.h>
#include <drivers/hpet.h>
#include <drivers/pic.h>
#include <drivers/pit.h>
#include <drivers/serial.h>
#include <drivers/vga.h>

#ifdef KTF_PMU
#include <perfmon/pfmlib.h>
#endif

boot_flags_t boot_flags;

static unsigned bsp_cpu_id = 0;

unsigned get_bsp_cpu_id(void) { return bsp_cpu_id; }
void set_bsp_cpu_id(unsigned cpu_id) { bsp_cpu_id = cpu_id; }

#define QEMU_CONSOLE_PORT 0x0e9

static void __text_init init_console(void) {
    uart_config_t cfg = {0};

    if (!parse_com_port(COM1, &cfg)) {
        /* Use first COM port indicated by BIOS (if none, use COM1) */
        cfg.port = get_first_com_port();
        cfg.baud = DEFAULT_BAUD_SPEED;
        cfg.frame_size = COM_FRAME_SIZE_8_BITS;
        cfg.parity = COM_NO_PARITY;
        cfg.stop_bit = COM_STOP_BIT_1;
    }
    init_uart(&cfg);
    register_console_callback(serial_console_write, _ptr(cfg.port));

    if (opt_qemu_console) {
        register_console_callback(qemu_console_write, _ptr(QEMU_CONSOLE_PORT));
        printk("Initialized QEMU console at port 0x%x", QEMU_CONSOLE_PORT);
    }

    printk("Serial console at: ");
    display_uart_config(&cfg);
}

static __always_inline void zero_bss(void) {
    memset(_ptr(__start_bss), 0x0, _ptr(__end_bss) - _ptr(__start_bss));
    memset(_ptr(__start_bss_user), 0x0, _ptr(__end_bss_user) - _ptr(__start_bss_user));
}

void zap_boot_mappings(void) {
    for_each_memory_range (r) {
        if (r->base == VIRT_IDENT_BASE && IS_INIT_SECTION(r->name)) {
            if (strcmp(r->name, ".text.init"))
                memset(r->start, 0, r->end - r->start);

            for (mfn_t mfn = virt_to_mfn(r->start); mfn < virt_to_mfn(r->end); mfn++) {
                vunmap(mfn_to_virt(mfn), PAGE_ORDER_4K);
                reclaim_frame(mfn, PAGE_ORDER_4K);
            }
        }
    }
}

static void __text_init map_bios_area(void) {
    vmap_4k(paddr_to_virt(BDA_ADDR_START), paddr_to_mfn(BDA_ADDR_START), L1_PROT_RO);
    kmap_4k(paddr_to_mfn(BDA_ADDR_START), L1_PROT_RO);

    uint32_t ebda_addr = get_bios_ebda_addr();
    vmap_4k(paddr_to_virt(ebda_addr), paddr_to_mfn(ebda_addr), L1_PROT_RO);
    kmap_4k(paddr_to_mfn(ebda_addr), L1_PROT_RO);

    for (mfn_t bios_mfn = paddr_to_mfn(BIOS_ACPI_ROM_START);
         bios_mfn < paddr_to_mfn(BIOS_ACPI_ROM_STOP); bios_mfn++)
        kmap_4k(bios_mfn, L1_PROT_RO);
}

static void display_cpu_info(void) {
    char cpu_identifier[49];
    unsigned long freq;

    if (!cpu_vendor_string(cpu_identifier))
        return;

    printk("CPU: %.48s\n", cpu_identifier);
    freq = get_cpu_freq(cpu_identifier);
    if (freq > 0)
        printk("Frequency: %lu MHz\n", freq / MHZ(1));
}

static void display_banner(void) { draw_logo(); }

static void __text_init init_vga_console(void) {
    if (!boot_flags.vga)
        return;

    printk("Enabling VGA support\n");
    map_vga_area();
    register_console_callback(vga_console_write, paddr_to_virt_kern(VGA_START_ADDR));
}

void __noreturn __text_init kernel_start(uint32_t multiboot_magic,
                                         multiboot_info_t *mbi) {
    /* Zero-out BSS sections */
    zero_bss();

    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        /* Indentity mapping is still on, so fill in multiboot structures */
        init_multiboot(mbi, &kernel_cmdline);
    }

    /* Parse commandline parameters */
    cmdline_parse(kernel_cmdline);
    if (!string_empty(kernel_cmdline))
        printk("Command line: %s\n", kernel_cmdline);

    /* Initialize console early */
    init_console();

    init_boot_traps();

    init_real_mode();

    /* Print cpu vendor info */
    display_cpu_info();

    /* Initialize Programmable Interrupt Controller */
    init_pic();

    /* PIC is initialized - enable local interrupts */
    sti();

    /* Initialize Physical Memory Manager */
    init_regions();
    init_pmm();

    /* Setup final pagetables */
    init_pagetables();

    map_multiboot_areas();
    map_bios_area();

    write_cr3(cr3.paddr);
    boot_flags.virt = true;

    WRITE_SP(get_free_pages_top(PAGE_ORDER_2M, GFP_KERNEL));
    if (opt_debug)
        dump_pagetables();

    if (init_framebuffer(mbi))
        display_banner();
    else
        init_vga_console();

    init_percpu();

    init_traps(get_bsp_cpu_id());

    init_extables();

    init_slab();

    /* Try to initialize ACPI (and MADT) */
#ifndef KTF_ACPICA
    if (init_acpi(get_bsp_cpu_id()) < 0) {
#else
    if (ACPI_FAILURE(init_acpi(get_bsp_cpu_id()))) {
#endif
        /* Fallback to MP tables when no ACPI */
        if (init_mptables() < 0)
            BUG();
    }

    init_apic(get_bsp_cpu_id(), APIC_MODE_XAPIC);

    init_tasks();

    init_smp();

    init_ioapic();

    init_pci();

    /* Initialize console input */
    init_uart_input(get_bsp_cpu_id());

    /* Initialize timers */
    bool hpet_initialized = false;
    if (opt_hpet)
        hpet_initialized = init_hpet(get_bsp_cpu_id());
    if (!hpet_initialized && opt_pit)
        init_pit(get_bsp_cpu_id());
    if (opt_apic_timer)
        init_apic_timer();

    /* Initialize keyboard */
    if (opt_keyboard)
        init_keyboard(get_bsp_cpu_id());

    if (opt_fpu) {
        printk("Enabling FPU instructions support\n");
        enable_fpu();
    }

#ifdef KTF_PMU
    printk("Initializing PFM library\n");

    int ret = pfm_initialize();
    if (ret != PFM_SUCCESS)
        printk("Warning: PFM library initialization failed: %d\n", ret);
#endif

    /* Jump from .text.init section to .text */
    asm volatile("jmp kernel_main");

    UNREACHABLE();
}
