/* EMBODIOS Task Management */
#ifndef _EMBODIOS_TASK_H
#define _EMBODIOS_TASK_H

#include <embodios/types.h>

/* Task structure forward declaration */
typedef struct task task_t;

/* Initialize scheduler */
void scheduler_init(void);

/* Create a new task */
task_t* task_create(const char *name, void (*entry)(void));

/* Schedule next task */
void schedule(void);

/* Get current task */
task_t* get_current_task(void);

/* Yield CPU to next task */
void task_yield(void);

/* Exit current task */
void task_exit(void);

#endif /* _EMBODIOS_TASK_H */