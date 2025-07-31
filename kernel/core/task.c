/* Minimal task scheduler for EMBODIOS */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/mm.h"

#define MAX_TASKS 16
#define TASK_STACK_SIZE 8192

/* Task states */
typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

/* Task structure */
typedef struct task {
    uint32_t tid;               /* Task ID */
    char name[32];              /* Task name */
    task_state_t state;         /* Current state */
    void *stack_base;           /* Stack base address */
    void *stack_pointer;        /* Current stack pointer */
    void (*entry)(void);        /* Entry point */
    struct task *next;          /* Next task in list */
} task_t;

/* Scheduler state */
static struct {
    task_t tasks[MAX_TASKS];
    task_t *current_task;
    task_t *task_list;
    uint32_t next_tid;
    bool initialized;
} sched_state = {
    .current_task = NULL,
    .task_list = NULL,
    .next_tid = 1,
    .initialized = false
};

/* Initialize scheduler */
void scheduler_init(void)
{
    /* Clear all tasks */
    for (int i = 0; i < MAX_TASKS; i++) {
        sched_state.tasks[i].state = TASK_DEAD;
        sched_state.tasks[i].tid = 0;
    }
    
    sched_state.initialized = true;
    console_printf("Scheduler: Initialized with %d task slots\n", MAX_TASKS);
}

/* Create a new task */
task_t* task_create(const char *name, void (*entry)(void))
{
    /* Find free task slot */
    task_t *task = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (sched_state.tasks[i].state == TASK_DEAD) {
            task = &sched_state.tasks[i];
            break;
        }
    }
    
    if (!task) {
        console_printf("Scheduler: No free task slots\n");
        return NULL;
    }
    
    /* Initialize task */
    task->tid = sched_state.next_tid++;
    strncpy(task->name, name, sizeof(task->name) - 1);
    task->entry = entry;
    task->state = TASK_READY;
    
    /* Allocate stack */
    task->stack_base = kmalloc(TASK_STACK_SIZE);
    if (!task->stack_base) {
        console_printf("Scheduler: Failed to allocate stack for task %s\n", name);
        task->state = TASK_DEAD;
        return NULL;
    }
    
    /* Set up initial stack */
    task->stack_pointer = (uint8_t*)task->stack_base + TASK_STACK_SIZE;
    
    /* Add to task list */
    task->next = sched_state.task_list;
    sched_state.task_list = task;
    
    console_printf("Scheduler: Created task '%s' (TID=%u)\n", name, task->tid);
    return task;
}

/* Simple round-robin scheduler */
void schedule(void)
{
    if (!sched_state.initialized || !sched_state.task_list) {
        return;
    }
    
    /* Find next ready task */
    task_t *next = sched_state.current_task ? sched_state.current_task->next : sched_state.task_list;
    if (!next) {
        next = sched_state.task_list;
    }
    
    /* Find a ready task */
    const task_t *start = next;
    while (next && next->state != TASK_READY) {
        next = next->next;
        if (!next) {
            next = sched_state.task_list;
        }
        if (next == start) {
            /* No ready tasks */
            return;
        }
    }
    
    /* Switch to next task */
    if (next != sched_state.current_task) {
        task_t *prev = sched_state.current_task;
        
        if (prev && prev->state == TASK_RUNNING) {
            prev->state = TASK_READY;
        }
        
        sched_state.current_task = next;
        next->state = TASK_RUNNING;
        
        /* In a real implementation, we would context switch here */
        /* For now, we'll just run tasks cooperatively */
    }
}

/* Get current task */
task_t* get_current_task(void)
{
    return sched_state.current_task;
}

/* Yield CPU to next task */
void task_yield(void)
{
    schedule();
}

/* Exit current task */
void task_exit(void)
{
    if (sched_state.current_task) {
        sched_state.current_task->state = TASK_DEAD;
        schedule();
    }
}