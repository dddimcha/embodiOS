/* EMBODIOS Real-Time Task Scheduler
 *
 * Priority-based preemptive scheduler with real-time support.
 * Implements priority scheduling with round-robin for equal priorities,
 * deadline-aware priority boosting, and priority inheritance protocol.
 *
 * Features:
 * - 32 priority levels (0=highest, 31=lowest)
 * - Preemptive scheduling with configurable time quantum
 * - Deadline-aware scheduling with automatic priority boosting
 * - Priority inheritance protocol for resource synchronization
 * - Nestable preemption control for critical sections
 * - Priority inversion detection and mitigation
 * - Comprehensive scheduling statistics
 *
 * Scheduling Algorithm:
 * 1. Priority-based: Higher priority tasks run first (0 > 31)
 * 2. Round-robin: Equal priority tasks share CPU time (10 tick quantum)
 * 3. Deadline boosting: Tasks approaching deadline (<10 ticks) boosted to priority 0
 * 4. Priority inheritance: Tasks holding resources inherit waiting task priority
 *
 * Reference: FreeRTOS scheduler, Linux CFS, PREEMPT_RT patches
 */

#ifndef _EMBODIOS_TASK_H
#define _EMBODIOS_TASK_H

#include <embodios/types.h>

/* ============================================================================
 * Task Types
 * ============================================================================ */

/* CPU affinity constants */
#define CPU_AFFINITY_ANY    0xFFFFFFFF  /* Task can run on any CPU */

/**
 * struct task - Task control block (opaque)
 *
 * This is an opaque type. Task internals are defined in task.c.
 */
typedef struct task task_t;

/* ============================================================================
 * Scheduler Initialization
 * ============================================================================ */

/**
 * scheduler_init - Initialize the task scheduler
 *
 * Initializes internal scheduler data structures and prepares the system
 * for task creation and scheduling. Must be called once during kernel
 * initialization before any tasks are created.
 */
void scheduler_init(void);

/**
 * scheduler_register_timer - Register scheduler with timer subsystem
 *
 * Connects the scheduler's tick handler to the system timer interrupt.
 * Enables preemptive scheduling by having the timer invoke scheduler_tick()
 * on each timer interrupt (typically 100Hz = 10ms tick).
 *
 * Must be called after scheduler_init() and timer initialization.
 */
void scheduler_register_timer(void);

/* ============================================================================
 * Task Management
 * ============================================================================ */

/**
 * task_create - Create a new task
 * @name: Human-readable task name (max 31 characters)
 * @entry: Task entry point function
 * @priority: Initial priority (0-31, 0=highest)
 *
 * Creates a new task with the specified priority. The task is allocated
 * a stack and added to the ready queue. Priorities are clamped to the
 * valid range 0-31.
 *
 * Returns: Pointer to task structure on success, NULL on failure
 */
task_t* task_create(const char *name, void (*entry)(void), uint8_t priority);

/**
 * get_current_task - Get currently running task
 *
 * Returns: Pointer to current task, or NULL if no task is running
 */
task_t* get_current_task(void);

/**
 * task_yield - Voluntarily yield CPU
 *
 * Current task yields the CPU to allow other ready tasks to run.
 * Task remains in ready state and will be rescheduled according to
 * its priority.
 */
void task_yield(void);

/**
 * task_exit - Terminate current task
 *
 * Marks the current task as dead, removes it from all scheduling queues,
 * and triggers immediate rescheduling. Task resources (stack) remain
 * allocated but can be reused for future tasks.
 */
void task_exit(void);

/* ============================================================================
 * CPU Affinity Management (SMP)
 * ============================================================================ */

void task_set_affinity(task_t *task, uint32_t cpu_mask);
uint32_t task_get_affinity(task_t *task);
void task_pin_to_cpu(task_t *task, uint32_t cpu_id);
uint32_t task_get_cpu(task_t *task);

/* ============================================================================
 * Core Scheduler Operations
 * ============================================================================ */

/**
 * schedule - Invoke the scheduler
 *
 * Selects the highest-priority ready task and switches to it.
 * If current task is still running, it's moved to ready state.
 * Checks deadlines and applies priority boosting before selection.
 *
 * This function implements the core scheduling algorithm:
 * 1. Check deadlines and boost priorities for tasks approaching deadline
 * 2. Move current task to ready queue if still runnable
 * 3. Select highest-priority ready task from priority queue
 * 4. Perform context switch (cooperative in current implementation)
 */
void schedule(void);

/* ============================================================================
 * Priority Management
 * ============================================================================ */

/**
 * task_set_priority - Change task priority
 * @task: Task to modify
 * @priority: New priority (0-31, 0=highest)
 *
 * Updates the task's priority. If task is in ready state, it's removed
 * from the ready queue and re-inserted with the new priority.
 * Priorities are clamped to the valid range 0-31.
 *
 * Note: This sets the base priority. Actual priority may be higher due
 * to deadline boosting or priority inheritance.
 */
void task_set_priority(task_t *task, uint8_t priority);

/**
 * task_get_priority - Get task priority
 * @task: Task to query
 *
 * Returns: Current effective priority (0-31), or 31 if task is NULL
 *
 * Note: Returns the current effective priority, which may differ from
 * the base priority due to deadline boosting or priority inheritance.
 */
uint8_t task_get_priority(task_t *task);

/* ============================================================================
 * Deadline Scheduling
 * ============================================================================ */

/**
 * task_set_deadline - Set task deadline
 * @task: Task to modify
 * @deadline_ticks: Absolute deadline in timer ticks (0=no deadline)
 *
 * Sets an absolute deadline for the task. Tasks with deadlines are tracked
 * in a deadline-ordered list. When a deadline approaches (<10 ticks), the
 * task's priority is automatically boosted to 0 (highest). If the deadline
 * is missed, a warning is logged and the deadline is cleared.
 *
 * Pass 0 to clear an existing deadline.
 */
void task_set_deadline(task_t *task, uint64_t deadline_ticks);

/**
 * task_get_deadline - Get task deadline
 * @task: Task to query
 *
 * Returns: Absolute deadline in ticks, or 0 if no deadline set
 */
uint64_t task_get_deadline(task_t *task);

/* ============================================================================
 * Preemption Control
 * ============================================================================ */

/**
 * scheduler_disable_preemption - Disable preemptive scheduling
 *
 * Prevents the scheduler from preempting the current task on timer ticks.
 * This is used to protect critical sections where context switches would
 * be unsafe. Calls are nestable - preemption is only re-enabled when the
 * disable count reaches zero.
 *
 * If a higher-priority task becomes ready while preemption is disabled,
 * the preemption is marked pending and will occur when preemption is
 * re-enabled.
 *
 * Warning: Keep preemption-disabled sections short to maintain real-time
 * responsiveness. Long critical sections can cause deadline misses.
 */
void scheduler_disable_preemption(void);

/**
 * scheduler_enable_preemption - Enable preemptive scheduling
 *
 * Re-enables preemptive scheduling. Calls are nestable and must balance
 * with scheduler_disable_preemption() calls. When the disable count
 * reaches zero, if a preemption was pending, it will occur immediately.
 */
void scheduler_enable_preemption(void);

/* ============================================================================
 * Diagnostics and Testing
 * ============================================================================ */

/**
 * scheduler_stats - Display scheduler statistics
 *
 * Prints comprehensive scheduler statistics including:
 * - Total context switches and preemptions
 * - Priority inversions detected
 * - Current task and time quantum remaining
 * - Task count by state (ready, running, blocked, dead)
 * - Preemption disable status
 */
void scheduler_stats(void);

/**
 * scheduler_test_init - Run scheduler test suite
 *
 * Initializes and runs a comprehensive test suite covering:
 * - Task creation with priorities
 * - Priority getter/setter operations
 * - Deadline management
 * - Priority clamping
 * - Preemption control API
 *
 * Results are printed to console with pass/fail status.
 */
void scheduler_test_init(void);

#endif /* _EMBODIOS_TASK_H */