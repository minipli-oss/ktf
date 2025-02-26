/*
 * Copyright © 2021 Amazon.com, Inc. or its affiliates.
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
#ifdef KTF_ACPICA
#include <acpi_ktf.h>
#include <cpu.h>
#include <ktf.h>
#include <mm/slab.h>
#include <pci_cfg.h>
#include <percpu.h>
#include <sched.h>
#include <semaphore.h>
#include <setup.h>
#include <smp/smp.h>
#include <spinlock.h>
#include <time.h>
#include <traps.h>

#include "acpi.h"

struct mapped_frame {
    struct list_head list;
    mfn_t mfn;
    uint64_t refcount;
};
typedef struct mapped_frame mapped_frame_t;

static list_head_t mapped_frames;

static spinlock_t map_lock = SPINLOCK_INIT;

/* General OS functions */

ACPI_STATUS AcpiOsInitialize(void) {
    dprintk("ACPI OS Initialization:\n");

    list_init(&mapped_frames);
    return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void) {
    mapped_frame_t *frame, *safe;

    dprintk("ACPI OS Termination:\n");

    list_for_each_entry_safe (frame, safe, &mapped_frames, list) {
        list_unlink(&frame->list);
        kfree(frame);
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info) {
    switch (Function) {
    case ACPI_SIGNAL_FATAL: {
        ACPI_SIGNAL_FATAL_INFO *info = Info;

        panic("ACPI: Received ACPI_SIGNAL_FATAL: Type: %u, Code: %u, Arg: %u",
              info ? info->Type : 0, info ? info->Code : 0, info ? info->Argument : 0);
    } break;
    case ACPI_SIGNAL_BREAKPOINT: {
        char *bp_msg = Info;

        printk("ACPI: Received ACPI_SIGNAL_BREAKPOINT: %s", bp_msg ?: "");
    } break;
    default:
        warning("ACPI: Unsupported ACPI signal: %u", Function);
        break;
    }

    return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue) {
    dprintk("ACPI Entering sleep state S%u.\n", SleepState);

    return AE_OK;
}

/* Memory and IO space read/write functions */

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width) {
    void *pa = _ptr(_paddr(Address));
    UINT64 val = 0;

    switch (Width) {
    case 8:
        val = *(uint8_t *) pa;
        break;
    case 16:
        val = *(uint16_t *) pa;
        break;
    case 32:
        val = *(uint32_t *) pa;
        break;
    case 64:
        val = *(uint64_t *) pa;
        break;
    default:
        return AE_BAD_PARAMETER;
    }

    *Value = val;
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width) {
    void *pa = _ptr(_paddr(Address));

    switch (Width) {
    case 8:
        *(uint8_t *) pa = (uint8_t) Value;
        break;
    case 16:
        *(uint16_t *) pa = (uint16_t) Value;
        break;
    case 32:
        *(uint32_t *) pa = (uint32_t) Value;
        break;
    case 64:
        *(uint64_t *) pa = (uint64_t) Value;
        break;
    default:
        return AE_BAD_PARAMETER;
    }

    return AE_OK;
}

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width) {
    switch (Width) {
    case 8:
        *Value = inb(Address);
        break;
    case 16:
        *Value = inw(Address);
        break;
    case 32:
        *Value = ind(Address);
        break;
    default:
        return AE_BAD_PARAMETER;
    }

    return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width) {
    switch (Width) {
    case 8:
        outb(Address, (uint8_t) Value);
        break;
    case 16:
        outw(Address, (uint16_t) Value);
        break;
    case 32:
        outd(Address, (uint32_t) Value);
        break;
    default:
        return AE_BAD_PARAMETER;
    }

    return AE_OK;
}

/* General Table handling functions */

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void) {
    ACPI_PHYSICAL_ADDRESS pa = 0;

    if (acpi_rsdp)
        pa = (ACPI_PHYSICAL_ADDRESS) acpi_rsdp;
    else
        AcpiFindRootPointer(&pa);

    return pa;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *PredefinedObject,
                                     ACPI_STRING *NewValue) {
    if (!NewValue)
        return AE_BAD_PARAMETER;

    *NewValue = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *ExistingTable,
                                ACPI_TABLE_HEADER **NewTable) {
    if (!NewTable)
        return AE_BAD_PARAMETER;

    *NewTable = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *ExistingTable,
                                        ACPI_PHYSICAL_ADDRESS *NewAddress,
                                        UINT32 *NewTableLength) {
    if (!NewAddress || !NewTableLength)
        return AE_BAD_PARAMETER;

    *NewAddress = _paddr(NULL);
    *NewTableLength = 0;

    return AE_OK;
}

/* Memory management functions */

void *AcpiOsAllocate(ACPI_SIZE Size) {
    return kmalloc(Size);
}

void AcpiOsFree(void *Memory) {
    kfree(Memory);
}

BOOLEAN AcpiOsReadable(void *Memory, ACPI_SIZE Length) {
    volatile bool success = false;
    char *mem;

    for (mfn_t mfn = virt_to_mfn(Memory); mfn <= virt_to_mfn((char *) Memory + Length);
         ++mfn) {
        success = false;
        mem = mfn_to_virt_map(mfn);
        asm volatile("1: movq ( %[mem] ), %%rax; movq $1, %[success];"
                     "2:" ASM_EXTABLE(1b, 2b)
                     : [ success ] "=m"(success)
                     : [ mem ] "r"(mem)
                     : "rax", "memory");
        if (!success)
            return false;
    }
    return success;
}

BOOLEAN AcpiOsWriteable(void *Memory, ACPI_SIZE Length) {
    volatile bool success = false;
    char *mem;

    for (mfn_t mfn = virt_to_mfn(Memory); mfn <= virt_to_mfn((char *) Memory + Length);
         ++mfn) {
        success = false;
        mem = mfn_to_virt_map(mfn);
        asm volatile("1: orq $0, ( %[mem] ); movq $1, %[success];"
                     "2:" ASM_EXTABLE(1b, 2b)
                     : [ success ] "=m"(success), [ mem ] "=r"(mem)
                     :
                     : "memory");
        if (!success)
            return false;
    }

    return success;
}

static inline mapped_frame_t *find_mapped_frame(mfn_t mfn) {
    mapped_frame_t *frame;

    list_for_each_entry (frame, &mapped_frames, list) {
        if (frame->mfn == mfn)
            return frame;
    }

    return NULL;
}

static inline void new_mapped_frame(mfn_t mfn) {
    mapped_frame_t *frame = kzalloc(sizeof(*frame));
    frame->mfn = mfn;
    frame->refcount = 1;
    list_add(&frame->list, &mapped_frames);
}

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS PhysicalAddress, ACPI_SIZE Length) {
    unsigned offset = PhysicalAddress & ~PAGE_MASK;
    unsigned num_pages = ((offset + Length) / PAGE_SIZE) + 1;
    mfn_t mfn = paddr_to_mfn(PhysicalAddress);
    void *va = NULL;

    spin_lock(&map_lock);
    for (unsigned i = 0; i < num_pages; i++, mfn++) {
        mapped_frame_t *frame = find_mapped_frame(mfn);
        void *_va;

        if (!frame) {
            _va = vmap_4k(mfn_to_virt_map(mfn), mfn, L1_PROT);
            if (!_va) {
                spin_unlock(&map_lock);
                return NULL;
            }
            new_mapped_frame(mfn);
        }
        else {
            frame->refcount++;
            _va = mfn_to_virt_map(mfn);
        }

        if (!va)
            va = _ptr(_ul(_va) + offset);
    }
    spin_unlock(&map_lock);

    return va;
}

void AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE Length) {
    unsigned offset = _ul(LogicalAddress) & ~PAGE_MASK;
    unsigned num_pages = ((offset + Length) / PAGE_SIZE) + 1;
    mfn_t mfn = virt_to_mfn(LogicalAddress);

    spin_lock(&map_lock);
    for (unsigned i = 0; i < num_pages; i++, mfn++) {
        mapped_frame_t *frame = find_mapped_frame(mfn);
        BUG_ON(!frame || frame->refcount == 0);

        if (--frame->refcount > 0)
            continue;

        vunmap_kern(mfn_to_virt_map(mfn), PAGE_ORDER_4K);
        list_unlink(&frame->list);
        kfree(frame);
    }
    spin_unlock(&map_lock);
}

/* Task management functions */

ACPI_THREAD_ID AcpiOsGetThreadId(void) {
    /* This should return non-zero task ID.
     * Currently assume task ID equals to CPU ID.
     */
    return smp_processor_id() + 1;
}

struct osd_exec_cb_wrapper {
    ACPI_OSD_EXEC_CALLBACK Function;
    void *Context;
};
typedef struct osd_exec_cb_wrapper osd_exec_cb_wrapper_t;

unsigned long _osd_exec_cb_wrapper(void *arg) {
    osd_exec_cb_wrapper_t *cb = arg;
    cb->Function(cb->Context);
    return 0;
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function,
                          void *Context) {
    static unsigned counter = 0;
    cpu_t *cpu = get_cpu(smp_processor_id());
    osd_exec_cb_wrapper_t cb;
    char name[40];
    task_t *task;

    snprintf(name, sizeof(name), "acpi_%u_%u_%u", Type, counter++, cpu->id);

    cb.Function = Function;
    cb.Context = Context;
    task = new_kernel_task(name, _osd_exec_cb_wrapper, &cb);
    if (!task)
        return AE_NO_MEMORY;

    set_task_group(task, TASK_GROUP_ACPI);
    schedule_task(task, cpu);

    return AE_OK;
}

void AcpiOsWaitEventsComplete(void) {
    cpu_t *cpu = get_cpu(smp_processor_id());

    wait_for_task_group(cpu, TASK_GROUP_ACPI);
}

/* Synchronization and locking functions */

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle) {
    spinlock_t *lock;

    if (!OutHandle)
        return AE_BAD_PARAMETER;

    lock = kmalloc(sizeof(*lock));
    if (!lock)
        return AE_NO_MEMORY;

    *lock = SPINLOCK_INIT;
    *OutHandle = lock;

    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle) {
    spinlock_t *lock = Handle;

    kfree((void *) lock);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle) {
    /* FIXME: CPU flags are currently not implemented */
    ACPI_CPU_FLAGS flags = 0;

    spin_lock(Handle);
    return flags;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags) {
    /* FIXME: CPU flags are currently not implemented */
    spin_unlock(Handle);
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits,
                                  ACPI_SEMAPHORE *OutHandle) {
    sem_t *sem;

    if (!OutHandle)
        return AE_BAD_PARAMETER;

    sem = kmalloc(sizeof(*sem));
    if (!sem)
        return AE_NO_MEMORY;

    sem_init(sem, InitialUnits);
    *OutHandle = sem;

    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle) {
    sem_t *sem = Handle;

    if (!sem)
        return AE_BAD_PARAMETER;

    kfree((void *) sem);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout) {
    if (!Handle)
        return AE_BAD_PARAMETER;

    if (Timeout == ACPI_DO_NOT_WAIT) {
        uint32_t val = sem_value(Handle);

        if (val < Units)
            return AE_TIME;
    }

    sem_wait_units(Handle, Units);

    return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units) {
    if (!Handle)
        return AE_BAD_PARAMETER;

    sem_post_units(Handle, Units);

    return AE_OK;
}

/* Time management functions */

void AcpiOsSleep(UINT64 Miliseconds) {
    msleep(Miliseconds);
}

/* FIXME: Return in correct 100ns units */
UINT64 AcpiOsGetTimer(void) {
    return get_timer_ticks();
}

/* FIXME: Use actual microseconds granularity */
void AcpiOsStall(UINT32 Microseconds) {
    for (unsigned long i = Microseconds * 1000; i > 0; i--)
        cpu_relax();
}

/* PCI Configuration read/write functions */

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Register, UINT64 *Value,
                                       UINT32 Width) {
    UINT64 value = 0;

    if (!PciId || !Value)
        return AE_BAD_PARAMETER;

    switch (Width) {
    case 8:
        value = pci_cfg_read8(PciId->Bus, PciId->Device, PciId->Function, Register);
        break;
    case 16:
        value = pci_cfg_read16(PciId->Bus, PciId->Device, PciId->Function, Register);
        break;
    case 32:
    /* FIXME: Add 64-bit handling */
    case 64:
        value = pci_cfg_read(PciId->Bus, PciId->Device, PciId->Function, Register);
        break;
    default:
        return AE_BAD_PARAMETER;
    }

    *Value = value;
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Register, UINT64 Value,
                                        UINT32 Width) {
    if (!PciId)
        return AE_BAD_PARAMETER;

    switch (Width) {
    case 8:
        pci_cfg_write8(PciId->Bus, PciId->Device, PciId->Function, Register,
                       (uint8_t) Value);
        break;
    case 16:
        pci_cfg_write16(PciId->Bus, PciId->Device, PciId->Function, Register,
                        (uint16_t) Value);
        break;
    case 32:
    /* FIXME: Add 64-bit handling */
    case 64:
        pci_cfg_write(PciId->Bus, PciId->Device, PciId->Function, Register,
                      (uint32_t) Value);
        break;
    default:
        return AE_BAD_PARAMETER;
    }

    return AE_OK;
}

/* ACPI interrupt handling functions */

extern void asm_interrupt_handler_acpi(void);
static bool acpi_irq_installed = false;
static uint32_t acpi_irq_num;
static ACPI_OSD_HANDLER acpi_irq_handler = NULL;
static void *acpi_irq_context = NULL;
static bool acpi_irq_handled = false;

void acpi_interrupt_handler(void) {
    uint32_t ret = acpi_irq_handler(acpi_irq_context);

    if (ret == ACPI_INTERRUPT_HANDLED)
        acpi_irq_handled = true;
    else if (ret == ACPI_INTERRUPT_NOT_HANDLED)
        acpi_irq_handled = false;
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptLevel, ACPI_OSD_HANDLER Handler,
                                          void *Context) {
    cpu_t *cpu = get_bsp_cpu();
    percpu_t *percpu = cpu->percpu;

    if (acpi_irq_installed)
        return AE_ALREADY_EXISTS;

    if (!Handler || InterruptLevel > MAX_INT)
        return AE_BAD_PARAMETER;

    acpi_irq_num = InterruptLevel;
    acpi_irq_handler = Handler;
    acpi_irq_context = Context;

    set_intr_gate(&percpu->idt[acpi_irq_num], __KERN_CS, _ul(asm_interrupt_handler_acpi),
                  GATE_DPL0, GATE_PRESENT, 1);
    barrier();

    acpi_irq_installed = true;
    return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptLevel,
                                         ACPI_OSD_HANDLER Handler) {
    cpu_t *cpu = get_bsp_cpu();
    percpu_t *percpu = cpu->percpu;

    if (!acpi_irq_installed)
        return AE_NOT_EXIST;

    if (!Handler || InterruptLevel > MAX_INT || InterruptLevel != acpi_irq_num)
        return AE_BAD_PARAMETER;

    if (Handler != _ptr(get_intr_handler(&percpu->idt[acpi_irq_num])))
        return AE_BAD_PARAMETER;

    set_intr_gate(&percpu->idt[acpi_irq_num], __KERN_CS, _ul(NULL), GATE_DPL0,
                  GATE_NOT_PRESENT, 0);
    barrier();

    acpi_irq_installed = false;
    return AE_OK;
}
#endif /* KTF_ACPICA */
