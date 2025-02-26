/*
 * Copyright (c) 2020 Amazon.com, Inc. or its affiliates.
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
#include <console.h>
#include <cpu.h>
#include <errno.h>
#include <ktf.h>
#include <lib.h>
#include <list.h>
#include <sched.h>
#include <setup.h>
#include <spinlock.h>
#include <string.h>
#include <usermode.h>

#include <smp/smp.h>

#include <mm/slab.h>
#include <mm/vmm.h>

static tid_t next_tid;

void init_tasks(void) {
    printk("Initializing tasks\n");

    next_tid = 0;
}

static const char *task_state_names[] = {
    [TASK_STATE_NEW] = "NEW",
    [TASK_STATE_READY] = "READY",
    [TASK_STATE_SCHEDULED] = "SCHEDULED",
    [TASK_STATE_RUNNING] = "RUNNING",
    [TASK_STATE_DONE] = "DONE",
};

#define PAGE_ORDER_TASK PAGE_ORDER_4K

static inline void set_task_state(task_t *task, task_state_t state) {
    ASSERT(task);

    dprintk("CPU[%u]: state transition %s -> %s\n", task->cpu->id,
            task_state_names[task->state], task_state_names[state]);

    ACCESS_ONCE(task->state) = state;
    smp_mb();
}

static inline task_state_t get_task_state(task_t *task) {
    task_state_t state;
    ASSERT(task);

    state = ACCESS_ONCE(task->state);
    smp_rmb();

    return state;
}

static task_t *create_task(void) {
    task_t *task = kzalloc(sizeof(*task));

    if (!task)
        return NULL;

    memset(task, 0, sizeof(*task));
    task->id = next_tid++;
    task->gid = TASK_GROUP_ALL;
    set_task_state(task, TASK_STATE_NEW);
    atomic_set(&task->exec_count, 0);
    set_task_once(task);

    return task;
}

/* The caller should never use the parameter again after calling this function */
static void destroy_task(task_t *task) {
    if (!task)
        return;

    spin_lock(&task->cpu->lock);
    list_unlink(&task->list);
    if (task->stack)
        put_page_top(task->stack);
    spin_unlock(&task->cpu->lock);

    kfree(task);
}

static int prepare_task(task_t *task, const char *name, task_func_t func, void *arg,
                        task_type_t type) {
    if (!task)
        return -EINVAL;

    ASSERT(get_task_state(task) <= TASK_STATE_READY);

    task->name = name;
    task->func = func;
    task->arg = arg;
    task->type = type;
    if (task->type == TASK_TYPE_USER)
        task->stack = get_free_page_top(GFP_USER);
    set_task_state(task, TASK_STATE_READY);
    return ESUCCESS;
}

static void wait_for_task_state(task_t *task, task_state_t state) {
    if (!task)
        return;

    while (get_task_state(task) != state)
        cpu_relax();
}

task_t *new_task(const char *name, task_func_t func, void *arg, task_type_t type) {
    task_t *task = create_task();

    if (!task)
        return NULL;

    if (unlikely(prepare_task(task, name, func, arg, type) != ESUCCESS)) {
        destroy_task(task);
        return NULL;
    }

    return task;
}

task_t *get_task_by_name(cpu_t *cpu, const char *name) {
    task_t *task;

    list_for_each_entry (task, &cpu->task_queue, list) {
        if (string_equal(task->name, name))
            return task;
    }

    return NULL;
}

static const char *task_repeat_string(task_repeat_t repeat) {
    static char buf[20] = {0};

    switch (repeat) {
    case TASK_REPEAT_ONCE:
        return "ONCE";
    case TASK_REPEAT_LOOP:
        return "LOOP";
    default:
        snprintf(buf, sizeof(buf), "%u times", repeat);
        return buf;
    }
}

int schedule_task(task_t *task, cpu_t *cpu) {
    ASSERT(task);

    if (!cpu) {
        warning("Unable to schedule task: %s. CPU does not exist.", task->name);
        return -EEXIST;
    }

    ASSERT(get_task_state(task) == TASK_STATE_READY);

    printk("CPU[%u]: Scheduling task %s[%u] (%s)\n", cpu->id, task->name, task->id,
           task_repeat_string(task->repeat));

    spin_lock(&cpu->lock);
    list_add_tail(&task->list, &cpu->task_queue);
    task->cpu = cpu;
    set_task_state(task, TASK_STATE_SCHEDULED);
    spin_unlock(&cpu->lock);

    return 0;
}

static void run_task(task_t *task) {
    if (!task)
        return;

    wait_for_task_state(task, TASK_STATE_SCHEDULED);

    if (atomic64_inc_return(&task->exec_count) == 0)
        printk("CPU[%u]: Running task %s[%u]\n", task->cpu->id, task->name, task->id);

    set_task_state(task, TASK_STATE_RUNNING);
    if (task->type == TASK_TYPE_USER)
        task->result = enter_usermode(task->func, task->arg, task->stack);
    else
        task->result = task->func(task->arg);
    set_task_state(task, TASK_STATE_DONE);
}

void wait_for_task_group(const cpu_t *cpu, task_group_t group) {
    task_t *task, *safe;
    bool busy;

    do {
        busy = false;

        list_for_each_entry_safe (task, safe, &cpu->task_queue, list) {
            /* When group is unspecified the functions waits for all tasks. */
            if (group != TASK_GROUP_ALL && task->gid != group)
                continue;

            if (get_task_state(task) != TASK_STATE_DONE) {
                busy = true;
                wait_for_task_state(task, TASK_STATE_DONE);
            }
        }
        cpu_relax();
    } while (busy);
}

void process_task_repeat(task_t *task) {
    switch (task->repeat) {
    case TASK_REPEAT_ONCE:
        printk("%s task '%s' finished on CPU[%u] with result %ld (Run: %lu times)\n",
               task->type == TASK_TYPE_KERNEL ? "Kernel" : "User", task->name,
               task->cpu->id, task->result, atomic_read(&task->exec_count));
        destroy_task(task);
        break;
    case TASK_REPEAT_LOOP:
        set_task_state(task, TASK_STATE_SCHEDULED);
        break;
    default:
        task->repeat--;
        set_task_state(task, TASK_STATE_SCHEDULED);
        break;
    }
}

void run_tasks(cpu_t *cpu) {
    task_t *task, *safe;

    if (!is_cpu_bsp(cpu))
        wait_cpu_unblocked(cpu);
    set_cpu_unfinished(cpu);

    do {
        list_for_each_entry_safe (task, safe, &cpu->task_queue, list) {
            switch (task->state) {
            case TASK_STATE_DONE:
                process_task_repeat(task);
                break;
            case TASK_STATE_SCHEDULED:
                run_task(task);
                break;
            default:
                BUG();
            }
            cpu_relax();
        }
    } while (!list_is_empty(&cpu->task_queue));

    if (!is_cpu_bsp(cpu))
        set_cpu_blocked(cpu);
    set_cpu_finished(cpu);
}
