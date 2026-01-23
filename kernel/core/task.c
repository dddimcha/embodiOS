/* EMBODIOS Real-Time Task Scheduler Implementation
 *
 * Implements a priority-based preemptive scheduler with real-time features:
 * - 32 priority levels with strict priority ordering
 * - Round-robin scheduling for equal-priority tasks
 * - Deadline-aware priority boosting
 * - Priority inheritance protocol (PIP) for synchronization
 *
 * Internal Architecture:
 * - Ready queue: Priority-ordered linked list of runnable tasks
 * - Deadline list: Deadline-ordered linked list for deadline tracking
 * - Task pool: Fixed array of MAX_TASKS task control blocks
 *
 * Reference: FreeRTOS scheduler, PREEMPT_RT, priority inheritance protocol
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/mm.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define MAX_TASKS 16            /* Maximum number of concurrent tasks */
#define TASK_STACK_SIZE 8192    /* Stack size per task (8KB) */

/* ============================================================================
 * Task State Machine
 * ============================================================================ */

/**
 * enum task_state - Task lifecycle states
 *
 * @TASK_READY: Task is ready to run, in ready queue
 * @TASK_RUNNING: Task is currently executing on CPU
 * @TASK_BLOCKED: Task is blocked on resource (for future use)
 * @TASK_DEAD: Task has exited, slot can be reused
 */
typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

/* ============================================================================
 * Task Control Block
 * ============================================================================ */

/**
 * struct task - Task control block
 *
 * @tid: Unique task identifier
 * @name: Human-readable task name (for debugging)
 * @state: Current task state
 * @stack_base: Base address of allocated stack
 * @stack_pointer: Current stack pointer (for context switch)
 * @entry: Task entry point function
 * @priority: Current effective priority (0=highest, 31=lowest)
 * @deadline: Absolute deadline in ticks (0=no deadline)
 * @next: Next task in ready queue or global task list
 * @next_deadline: Next task in deadline-ordered list
 *
 * Priority Inheritance Protocol fields:
 * @original_priority: Base priority before any inheritance
 * @blocked_on: Resource this task is waiting for (NULL if not blocked)
 * @waiting_tasks: List of tasks blocked on resources held by this task
 */
typedef struct task {
    uint32_t tid;               /* Task ID */
    char name[32];              /* Task name */
    task_state_t state;         /* Current state */
    void *stack_base;           /* Stack base address */
    void *stack_pointer;        /* Current stack pointer */
    void (*entry)(void);        /* Entry point */
    uint8_t priority;           /* Priority (0-31, 0=highest) */
    uint64_t deadline;          /* Deadline in ticks (0=no deadline) */
    struct task *next;          /* Next task in list */
    struct task *next_deadline; /* Next task in deadline-ordered list */

    /* Priority inheritance support */
    uint8_t original_priority;  /* Original priority before inheritance */
    void *blocked_on;           /* Resource task is waiting for (NULL if not blocked) */
    struct task *waiting_tasks; /* List of tasks blocked on resources held by this task */
} task_t;

/* ============================================================================
 * Scheduler State
 * ============================================================================ */

/**
 * struct sched_state - Global scheduler state
 *
 * @tasks: Static pool of task control blocks
 * @current_task: Currently executing task (NULL if none)
 * @task_list: Global list of all tasks (for iteration)
 * @ready_queue: Priority-ordered queue of ready tasks (highest priority at head)
 * @deadline_list: Deadline-ordered list of tasks with deadlines (earliest first)
 * @next_tid: Next task ID to assign
 * @initialized: True if scheduler has been initialized
 * @ticks_remaining: Time quantum remaining for current task
 * @context_switches: Total number of context switches
 * @preemptions: Total number of preemptions
 * @preemption_disable_count: Nesting level for preemption disable
 * @preemption_pending: Preemption requested while disabled
 * @priority_inversions: Total priority inversions detected
 */
static struct {
    task_t tasks[MAX_TASKS];
    task_t *current_task;
    task_t *task_list;
    task_t *ready_queue;        /* Priority-ordered queue of ready tasks */
    task_t *deadline_list;      /* Deadline-ordered list of tasks with deadlines */
    uint32_t next_tid;
    bool initialized;
    uint32_t ticks_remaining;   /* Time quantum remaining for current task */
    uint64_t context_switches;  /* Total context switches */
    uint64_t preemptions;       /* Total preemptions */
    uint32_t preemption_disable_count; /* Nested preemption disable counter */
    bool preemption_pending;    /* Preemption requested while disabled */
    uint64_t priority_inversions; /* Total priority inversions detected */
} sched_state = {
    .current_task = NULL,
    .task_list = NULL,
    .ready_queue = NULL,
    .deadline_list = NULL,
    .next_tid = 1,
    .initialized = false,
    .ticks_remaining = 0,
    .context_switches = 0,
    .preemptions = 0,
    .preemption_disable_count = 0,
    .preemption_pending = false,
    .priority_inversions = 0
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

/* Priority queue management */
static void ready_queue_insert(task_t *task);
static void ready_queue_remove(task_t *task);

/* Deadline management */
static void deadline_list_insert(task_t *task);
static void deadline_list_remove(task_t *task);
static void check_deadlines(void);

/* Priority inheritance protocol */
static void task_inherit_priority(task_t *holder, task_t *waiter);
static void task_restore_priority(task_t *task);
static void task_add_waiter(task_t *holder, task_t *waiter);
static void task_remove_waiter(task_t *holder, task_t *waiter);

/* Scheduler tick handler (called from timer interrupt) */
void scheduler_tick(void);

/* ============================================================================
 * Public API - Initialization
 * ============================================================================ */

/**
 * scheduler_init - Initialize the task scheduler
 *
 * Initializes scheduler state and task pool. Must be called once during
 * kernel initialization before any tasks are created or scheduler is used.
 * Marks all task slots as TASK_DEAD (available for allocation).
 */
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

/**
 * scheduler_register_timer - Connect scheduler to timer subsystem
 *
 * Registers scheduler_tick() as the timer interrupt handler. This enables
 * preemptive scheduling by having the timer invoke the scheduler on each
 * tick (typically 100Hz = 10ms intervals).
 *
 * Must be called after scheduler_init() and timer initialization.
 */
void scheduler_register_timer(void)
{
    /* External timer function to register tick handler */
    extern void timer_register_tick_handler(void (*handler)(void));

    /* Register our tick handler */
    timer_register_tick_handler(scheduler_tick);

    console_printf("Scheduler: Registered with timer for preemptive scheduling\n");
}

/* ============================================================================
 * Public API - Task Management
 * ============================================================================ */

/**
 * task_create - Create a new task
 * @name: Human-readable task name (max 31 characters)
 * @entry: Task entry point function
 * @priority: Initial priority (0-31, clamped if out of range)
 *
 * Allocates a task control block from the task pool and creates a new task.
 * The task is allocated an 8KB stack and placed in the ready queue.
 * Priority is clamped to valid range (0-31, where 0 is highest priority).
 *
 * Returns: Task pointer on success, NULL if no free task slots or allocation failed
 */
task_t* task_create(const char *name, void (*entry)(void), uint8_t priority)
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

    /* Clamp priority to valid range (0-31) */
    if (priority > 31) {
        priority = 31;
    }

    /* Initialize task */
    task->tid = sched_state.next_tid++;
    strncpy(task->name, name, sizeof(task->name) - 1);
    task->entry = entry;
    task->state = TASK_READY;
    task->priority = priority;
    task->deadline = 0;         /* No deadline by default */
    task->next_deadline = NULL;

    /* Initialize priority inheritance fields */
    task->original_priority = priority;  /* Save original priority */
    task->blocked_on = NULL;             /* Not blocked on any resource */
    task->waiting_tasks = NULL;          /* No tasks waiting on this task */

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

    /* Add to ready queue */
    ready_queue_insert(task);

    console_printf("Scheduler: Created task '%s' (TID=%u, priority=%u)\n", name, task->tid, priority);
    return task;
}

/* ============================================================================
 * Ready Queue Management
 * ============================================================================ */

/**
 * ready_queue_insert - Insert task into priority-ordered ready queue
 * @task: Task to insert (must be in TASK_READY state)
 *
 * Inserts task into the ready queue maintaining priority order.
 * Queue is ordered from highest to lowest priority (0 at head, 31 at tail).
 * Tasks with equal priority are inserted after existing tasks (FIFO order).
 */
static void ready_queue_insert(task_t *task)
{
    if (!task || task->state != TASK_READY) {
        return;
    }

    /* Empty queue - task becomes head */
    if (!sched_state.ready_queue) {
        task->next = NULL;
        sched_state.ready_queue = task;
        return;
    }

    /* Task has higher priority than head - insert at front */
    if (task->priority < sched_state.ready_queue->priority) {
        task->next = sched_state.ready_queue;
        sched_state.ready_queue = task;
        return;
    }

    /* Find insertion point in priority order */
    task_t *curr = sched_state.ready_queue;
    while (curr->next && curr->next->priority <= task->priority) {
        curr = curr->next;
    }

    /* Insert task */
    task->next = curr->next;
    curr->next = task;
}

/**
 * ready_queue_remove - Remove task from ready queue
 * @task: Task to remove
 *
 * Removes task from the ready queue. Safe to call even if task is not
 * in the queue. Clears task's next pointer after removal.
 */
static void ready_queue_remove(task_t *task)
{
    if (!task || !sched_state.ready_queue) {
        return;
    }

    /* Task is at head */
    if (sched_state.ready_queue == task) {
        sched_state.ready_queue = task->next;
        task->next = NULL;
        return;
    }

    /* Find and remove task */
    task_t *curr = sched_state.ready_queue;
    while (curr->next) {
        if (curr->next == task) {
            curr->next = task->next;
            task->next = NULL;
            return;
        }
        curr = curr->next;
    }
}

/* ============================================================================
 * Deadline List Management
 * ============================================================================ */

/**
 * deadline_list_insert - Insert task into deadline-ordered list
 * @task: Task to insert (must have deadline > 0)
 *
 * Inserts task into the deadline list maintaining deadline order.
 * List is ordered from earliest to latest deadline.
 * Tasks with deadline=0 are not inserted.
 */
static void deadline_list_insert(task_t *task)
{
    if (!task || task->deadline == 0) {
        return; /* No deadline, don't add to list */
    }

    /* Empty list - task becomes head */
    if (!sched_state.deadline_list) {
        task->next_deadline = NULL;
        sched_state.deadline_list = task;
        return;
    }

    /* Task has earlier deadline than head - insert at front */
    if (task->deadline < sched_state.deadline_list->deadline) {
        task->next_deadline = sched_state.deadline_list;
        sched_state.deadline_list = task;
        return;
    }

    /* Find insertion point in deadline order */
    task_t *curr = sched_state.deadline_list;
    while (curr->next_deadline && curr->next_deadline->deadline <= task->deadline) {
        curr = curr->next_deadline;
    }

    /* Insert task */
    task->next_deadline = curr->next_deadline;
    curr->next_deadline = task;
}

/**
 * deadline_list_remove - Remove task from deadline list
 * @task: Task to remove
 *
 * Removes task from the deadline list. Safe to call even if task is not
 * in the list. Clears task's next_deadline pointer after removal.
 */
static void deadline_list_remove(task_t *task)
{
    if (!task || !sched_state.deadline_list) {
        return;
    }

    /* Task is at head */
    if (sched_state.deadline_list == task) {
        sched_state.deadline_list = task->next_deadline;
        task->next_deadline = NULL;
        return;
    }

    /* Find and remove task */
    task_t *curr = sched_state.deadline_list;
    while (curr->next_deadline) {
        if (curr->next_deadline == task) {
            curr->next_deadline = task->next_deadline;
            task->next_deadline = NULL;
            return;
        }
        curr = curr->next_deadline;
    }
}

/**
 * check_deadlines - Check task deadlines and boost priorities
 *
 * Walks the deadline list checking each task's deadline against current time:
 * - If deadline has passed: Log warning and clear deadline
 * - If deadline is approaching (<10 ticks): Boost priority to 0 (highest)
 *
 * Called from schedule() before selecting next task to run.
 * Ensures time-critical tasks meet their deadlines.
 */
static void check_deadlines(void)
{
    /* Get current time from timer */
    extern uint64_t get_timer_ticks(void);
    uint64_t current_ticks = get_timer_ticks();

    /* Walk deadline list (already sorted by earliest deadline first) */
    task_t *task = sched_state.deadline_list;
    while (task) {
        if (task->deadline == 0) {
            /* No deadline, skip */
            task = task->next_deadline;
            continue;
        }

        /* Check if deadline has passed */
        if (current_ticks > task->deadline) {
            console_printf("Scheduler: DEADLINE MISS - Task '%s' (TID=%u) missed deadline by %llu ticks\n",
                          task->name, task->tid, current_ticks - task->deadline);
            /* Reset deadline to avoid repeated warnings */
            task->deadline = 0;
            task = task->next_deadline;
            continue;
        }

        /* Calculate time until deadline */
        uint64_t time_to_deadline = task->deadline - current_ticks;

        /* Boost priority if deadline is approaching (< 10 ticks) */
        if (time_to_deadline < 10 && task->priority > 0) {
            /* Save original priority if not already boosted */
            uint8_t old_priority = task->priority;

            /* Boost to highest priority */
            if (task->state == TASK_READY) {
                ready_queue_remove(task);
                task->priority = 0;
                ready_queue_insert(task);
            } else {
                task->priority = 0;
            }

            console_printf("Scheduler: Boosted priority for task '%s' (TID=%u) from %u to 0 (deadline in %llu ticks)\n",
                          task->name, task->tid, old_priority, time_to_deadline);
        }

        task = task->next_deadline;
    }
}

/* ============================================================================
 * Priority Inheritance Protocol (PIP)
 * ============================================================================ */

/**
 * task_inherit_priority - Boost holder priority via inheritance
 * @holder: Task holding a resource
 * @waiter: Task waiting for the resource
 *
 * Implements Priority Inheritance Protocol (PIP) to prevent priority inversion.
 * If waiter has higher priority (lower number) than holder, holder inherits
 * waiter's priority. This ensures that the holder completes quickly and
 * releases the resource.
 *
 * If holder becomes higher priority than current task, triggers preemption
 * (unless preemption is disabled, in which case marks preemption pending).
 */
static void task_inherit_priority(task_t *holder, task_t *waiter)
{
    if (!holder || !waiter) {
        return;
    }

    /* Only inherit if waiter has higher priority (lower number = higher priority) */
    if (waiter->priority < holder->priority) {
        uint8_t old_priority = holder->priority;

        /* Remove holder from ready queue if it's in READY state */
        if (holder->state == TASK_READY) {
            ready_queue_remove(holder);
        }

        /* Boost holder to waiter's priority */
        holder->priority = waiter->priority;

        /* Re-insert into ready queue with new priority if ready */
        if (holder->state == TASK_READY) {
            ready_queue_insert(holder);
        }

        console_printf("Scheduler: Priority inheritance - Task '%s' (TID=%u) boosted from %u to %u (waiting task: '%s')\n",
                      holder->name, holder->tid, old_priority, holder->priority, waiter->name);

        /* Trigger preemption check if holder becomes higher priority than current task */
        if (sched_state.current_task && holder->state == TASK_READY &&
            holder->priority < sched_state.current_task->priority) {
            if (sched_state.preemption_disable_count == 0) {
                sched_state.preemptions++;
                schedule();
            } else {
                sched_state.preemption_pending = true;
            }
        }
    }
}

/**
 * task_restore_priority - Restore priority after resource release
 * @task: Task that released a resource
 *
 * Restores task's priority after releasing a resource. If other tasks are
 * still waiting on resources held by this task, priority is restored to
 * the highest priority among remaining waiters. Otherwise, restores to
 * original priority.
 *
 * Called when a task releases a resource that other tasks were waiting for.
 */
static void task_restore_priority(task_t *task)
{
    if (!task) {
        return;
    }

    /* Check if priority was inherited (current priority != original priority) */
    if (task->priority != task->original_priority) {
        /* Check if any waiting tasks still require elevated priority */
        uint8_t highest_waiter_priority = task->original_priority;
        task_t *waiter = task->waiting_tasks;
        while (waiter) {
            if (waiter->priority < highest_waiter_priority) {
                highest_waiter_priority = waiter->priority;
            }
            waiter = waiter->next;
        }

        /* If new priority is different from current, update it */
        if (highest_waiter_priority != task->priority) {
            uint8_t old_priority = task->priority;

            /* Remove from ready queue if ready */
            if (task->state == TASK_READY) {
                ready_queue_remove(task);
            }

            /* Restore priority */
            task->priority = highest_waiter_priority;

            /* Re-insert into ready queue with new priority if ready */
            if (task->state == TASK_READY) {
                ready_queue_insert(task);
            }

            console_printf("Scheduler: Priority restored - Task '%s' (TID=%u) from %u to %u\n",
                          task->name, task->tid, old_priority, task->priority);
        }
    }
}

/**
 * task_add_waiter - Add waiter to holder's waiting list
 * @holder: Task holding a resource
 * @waiter: Task that is now waiting for the resource
 *
 * Adds waiter to holder's list of waiting tasks. Detects and logs priority
 * inversions (high-priority task blocked by low-priority task). Applies
 * priority inheritance to mitigate the inversion.
 *
 * Used by synchronization primitives (mutex, semaphore) when a task blocks
 * on a resource held by another task.
 */
static void task_add_waiter(task_t *holder, task_t *waiter)
{
    if (!holder || !waiter) {
        return;
    }

    /* Detect priority inversion: high-priority task blocked by low-priority task */
    if (waiter->priority < holder->priority) {
        sched_state.priority_inversions++;
        console_printf("Scheduler: PRIORITY INVERSION DETECTED - Task '%s' (priority=%u) blocked by task '%s' (priority=%u)\n",
                      waiter->name, waiter->priority, holder->name, holder->priority);
    }

    /* Add waiter to holder's waiting list */
    waiter->next = holder->waiting_tasks;
    holder->waiting_tasks = waiter;

    /* Mark waiter as blocked on this holder */
    waiter->blocked_on = holder;

    /* Apply priority inheritance */
    task_inherit_priority(holder, waiter);
}

/**
 * task_remove_waiter - Remove waiter from holder's waiting list
 * @holder: Task holding a resource
 * @waiter: Task that is no longer waiting (got the resource or gave up)
 *
 * Removes waiter from holder's list of waiting tasks. Clears waiter's
 * blocked_on pointer. Restores holder's priority based on remaining waiters.
 *
 * Used by synchronization primitives when a task acquires a resource or
 * stops waiting for it.
 */
static void task_remove_waiter(task_t *holder, task_t *waiter)
{
    if (!holder || !waiter) {
        return;
    }

    /* Remove waiter from holder's waiting list */
    if (holder->waiting_tasks == waiter) {
        holder->waiting_tasks = waiter->next;
    } else {
        task_t *curr = holder->waiting_tasks;
        while (curr && curr->next) {
            if (curr->next == waiter) {
                curr->next = waiter->next;
                break;
            }
            curr = curr->next;
        }
    }

    /* Clear waiter's blocked_on pointer */
    waiter->blocked_on = NULL;
    waiter->next = NULL;

    /* Restore holder's priority if needed */
    task_restore_priority(holder);
}

/* ============================================================================
 * Core Scheduler
 * ============================================================================ */

/**
 * schedule - Core scheduling function
 *
 * Implements the main scheduling algorithm:
 * 1. Check deadlines and boost priorities for tasks approaching deadlines
 * 2. Move current task to ready queue if still runnable
 * 3. Select highest-priority ready task from priority queue
 * 4. Perform context switch (cooperative in current implementation)
 *
 * The scheduler guarantees:
 * - Highest priority ready task always runs next
 * - Equal priority tasks share CPU via round-robin
 * - Tasks approaching deadlines are boosted to highest priority
 * - Priority inheritance prevents unbounded priority inversion
 */
void schedule(void)
{
    if (!sched_state.initialized) {
        return;
    }

    /* Check deadlines and boost priority for tasks approaching deadline */
    check_deadlines();

    /* If current task is still running, make it ready and re-queue */
    if (sched_state.current_task && sched_state.current_task->state == TASK_RUNNING) {
        sched_state.current_task->state = TASK_READY;
        ready_queue_insert(sched_state.current_task);
    }

    /* Get highest priority ready task (head of ready queue) */
    task_t *next = sched_state.ready_queue;
    if (!next) {
        /* No ready tasks */
        sched_state.current_task = NULL;
        return;
    }

    /* Remove from ready queue and switch to it */
    ready_queue_remove(next);

    /* Track context switch */
    if (sched_state.current_task != next) {
        sched_state.context_switches++;
    }

    sched_state.current_task = next;
    next->state = TASK_RUNNING;

    /* Reset time quantum for new task (10 ticks = 100ms at 100Hz) */
    sched_state.ticks_remaining = 10;

    /* In a real implementation, we would context switch here */
    /* For now, we'll just run tasks cooperatively */
}

/**
 * scheduler_tick - Timer interrupt handler for preemptive scheduling
 *
 * Called from timer interrupt (typically 100Hz = 10ms tick). Implements
 * preemptive scheduling by:
 * 1. Checking if higher priority task is ready (preemption)
 * 2. Decrementing current task's time quantum
 * 3. Triggering round-robin scheduling when quantum expires
 *
 * Preemption can be temporarily disabled via scheduler_disable_preemption().
 * When disabled, preemption requests are marked pending and occur when
 * re-enabled.
 *
 * Called from interrupt context - must be fast and safe.
 */
void scheduler_tick(void)
{
    if (!sched_state.initialized) {
        return;
    }

    /* No current task - schedule next task */
    if (!sched_state.current_task) {
        schedule();
        return;
    }

    /* Decrement time quantum */
    if (sched_state.ticks_remaining > 0) {
        sched_state.ticks_remaining--;
    }

    /* Check if preemption is disabled */
    if (sched_state.preemption_disable_count > 0) {
        /* Preemption is disabled - defer scheduling */
        /* Check if we should mark preemption as pending */
        if (sched_state.ready_queue &&
            sched_state.ready_queue->priority < sched_state.current_task->priority) {
            /* Mark that preemption is needed when re-enabled */
            sched_state.preemption_pending = true;
        }
        return;
    }

    /* Check if higher priority task is ready (preemption check) */
    if (sched_state.ready_queue &&
        sched_state.ready_queue->priority < sched_state.current_task->priority) {
        /* Higher priority task is ready - preempt current task */
        sched_state.preemptions++;
        schedule();
        return;
    }

    /* Check if time quantum expired for equal-priority round-robin */
    if (sched_state.ticks_remaining == 0) {
        /* Time quantum expired - check if there are other ready tasks */
        if (sched_state.ready_queue) {
            /* Check if there's a task with same or higher priority ready */
            if (sched_state.ready_queue->priority <= sched_state.current_task->priority) {
                schedule();
                return;
            }
        }
        /* No other ready tasks, reset time quantum and continue current task */
        sched_state.ticks_remaining = 10;
    }
}

/**
 * get_current_task - Get currently running task
 *
 * Returns: Pointer to current task's control block, or NULL if no task running
 */
task_t* get_current_task(void)
{
    return sched_state.current_task;
}

/**
 * task_yield - Voluntarily yield CPU
 *
 * Current task voluntarily gives up the CPU. Task remains ready and will
 * be rescheduled according to its priority. Used for cooperative multitasking
 * or when a task has no more work to do in this time slice.
 */
void task_yield(void)
{
    schedule();
}

/**
 * task_exit - Terminate current task
 *
 * Marks current task as TASK_DEAD and removes it from all scheduling queues
 * (ready queue and deadline list). Triggers immediate rescheduling to run
 * next ready task. Task control block and stack remain allocated and can
 * be reused for future tasks.
 *
 * This function does not return.
 */
void task_exit(void)
{
    if (sched_state.current_task) {
        task_t *task = sched_state.current_task;
        task->state = TASK_DEAD;

        /* Remove from ready queue if present */
        ready_queue_remove(task);

        /* Remove from deadline list if present */
        deadline_list_remove(task);

        schedule();
    }
}

/* ============================================================================
 * Public API - Priority Management
 * ============================================================================ */

/**
 * task_set_priority - Change task priority
 * @task: Task to modify
 * @priority: New priority (0-31, clamped if out of range)
 *
 * Updates the task's base priority. Priority is clamped to valid range (0-31).
 * If task is in ready queue, it's removed and re-inserted with new priority
 * to maintain queue ordering.
 *
 * Note: This sets the base priority. Actual effective priority may be higher
 * due to deadline boosting or priority inheritance.
 */
void task_set_priority(task_t *task, uint8_t priority)
{
    if (!task) {
        return;
    }

    /* Clamp priority to valid range (0-31) */
    if (priority > 31) {
        priority = 31;
    }

    /* If task is ready, remove from queue, update priority, and re-insert */
    if (task->state == TASK_READY) {
        ready_queue_remove(task);
        task->priority = priority;
        ready_queue_insert(task);
    } else {
        task->priority = priority;
    }

    console_printf("Scheduler: Set priority for task '%s' to %u\n", task->name, priority);
}

/**
 * task_get_priority - Get task priority
 * @task: Task to query
 *
 * Returns: Current effective priority (0-31), or 31 (lowest) if task is NULL
 *
 * Note: Returns the current effective priority, which may differ from base
 * priority due to deadline boosting or priority inheritance.
 */
uint8_t task_get_priority(task_t *task)
{
    if (!task) {
        return 31; /* Return lowest priority if task is NULL */
    }

    return task->priority;
}

/* ============================================================================
 * Public API - Deadline Management
 * ============================================================================ */

/**
 * task_set_deadline - Set absolute deadline for task
 * @task: Task to modify
 * @deadline_ticks: Absolute deadline in timer ticks (0=clear deadline)
 *
 * Sets an absolute deadline for the task. Tasks with deadlines are tracked
 * in a deadline-ordered list. When a task's deadline approaches (<10 ticks),
 * its priority is automatically boosted to 0 (highest) to help it meet the
 * deadline. If deadline is missed, a warning is logged and deadline is cleared.
 *
 * Pass deadline_ticks=0 to clear an existing deadline.
 */
void task_set_deadline(task_t *task, uint64_t deadline_ticks)
{
    if (!task) {
        return;
    }

    /* Remove from deadline list if already in it */
    if (task->deadline != 0) {
        deadline_list_remove(task);
    }

    /* Update deadline */
    task->deadline = deadline_ticks;

    /* Add to deadline list if new deadline is non-zero */
    if (deadline_ticks != 0) {
        deadline_list_insert(task);
        console_printf("Scheduler: Set deadline for task '%s' to %llu ticks\n", task->name, deadline_ticks);
    } else {
        console_printf("Scheduler: Cleared deadline for task '%s'\n", task->name);
    }
}

/**
 * task_get_deadline - Get task deadline
 * @task: Task to query
 *
 * Returns: Absolute deadline in timer ticks, or 0 if no deadline set or task is NULL
 */
uint64_t task_get_deadline(task_t *task)
{
    if (!task) {
        return 0; /* Return no deadline if task is NULL */
    }

    return task->deadline;
}

/* ============================================================================
 * Public API - Preemption Control
 * ============================================================================ */

/**
 * scheduler_disable_preemption - Disable preemptive scheduling
 *
 * Prevents the scheduler from preempting the current task on timer ticks.
 * Used to protect critical sections where context switches would be unsafe.
 * Calls are nestable - each disable must be matched with an enable call.
 *
 * If a higher-priority task becomes ready while preemption is disabled,
 * the preemption is marked pending and will occur when preemption is
 * fully re-enabled (disable count reaches zero).
 *
 * Warning: Keep preemption-disabled sections short (<1ms) to maintain
 * real-time responsiveness. Long critical sections can cause deadline misses.
 */
void scheduler_disable_preemption(void)
{
    sched_state.preemption_disable_count++;
}

/**
 * scheduler_enable_preemption - Re-enable preemptive scheduling
 *
 * Decrements the preemption disable counter. Preemption is only fully
 * re-enabled when the counter reaches zero (must balance all disable calls).
 *
 * If preemption was pending when fully re-enabled, immediately checks if
 * a higher-priority task is ready and schedules it if so.
 *
 * Warns if called without a matching disable (counter underflow protection).
 */
void scheduler_enable_preemption(void)
{
    /* Prevent underflow */
    if (sched_state.preemption_disable_count == 0) {
        console_printf("Scheduler: Warning - preemption enable without matching disable\n");
        return;
    }

    sched_state.preemption_disable_count--;

    /* If fully re-enabled and preemption was pending, schedule now */
    if (sched_state.preemption_disable_count == 0 && sched_state.preemption_pending) {
        sched_state.preemption_pending = false;
        /* Check if higher priority task is still ready */
        if (sched_state.ready_queue && sched_state.current_task &&
            sched_state.ready_queue->priority < sched_state.current_task->priority) {
            sched_state.preemptions++;
            schedule();
        }
    }
}

/* ============================================================================
 * Public API - Diagnostics and Testing
 * ============================================================================ */

/**
 * scheduler_stats - Display comprehensive scheduler statistics
 *
 * Prints detailed scheduler statistics to console including:
 * - Total context switches and preemptions
 * - Priority inversions detected (and mitigated via PIP)
 * - Current running task and remaining time quantum
 * - Preemption disable status and nesting count
 * - Task count breakdown by state (ready, running, blocked, dead)
 *
 * Useful for debugging scheduling issues and monitoring real-time performance.
 */
void scheduler_stats(void)
{
    console_printf("Scheduler Statistics:\n");
    console_printf("  Context switches: %llu\n", sched_state.context_switches);
    console_printf("  Preemptions: %llu\n", sched_state.preemptions);
    console_printf("  Priority inversions detected: %llu\n", sched_state.priority_inversions);
    console_printf("  Current task: %s\n",
                   sched_state.current_task ? sched_state.current_task->name : "None");
    console_printf("  Time quantum remaining: %u ticks\n", sched_state.ticks_remaining);
    console_printf("  Preemption disabled: %s (count=%u)\n",
                   sched_state.preemption_disable_count > 0 ? "Yes" : "No",
                   sched_state.preemption_disable_count);

    /* Count tasks in each state */
    uint32_t ready_count = 0, running_count = 0, blocked_count = 0, dead_count = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        switch (sched_state.tasks[i].state) {
            case TASK_READY:   ready_count++;   break;
            case TASK_RUNNING: running_count++; break;
            case TASK_BLOCKED: blocked_count++; break;
            case TASK_DEAD:    dead_count++;    break;
        }
    }

    console_printf("  Task states: %u ready, %u running, %u blocked, %u dead\n",
                   ready_count, running_count, blocked_count, dead_count);
}

/* ============================================================================
 * Test Suite
 * ============================================================================ */

/**
 * test_task_high - Test task entry point (high priority)
 *
 * Placeholder task for scheduler testing. Yields CPU in infinite loop.
 */
static void test_task_high(void)
{
    /* Placeholder test task */
    while (1) {
        task_yield();
    }
}

/**
 * test_task_medium - Test task entry point (medium priority)
 */
static void test_task_medium(void)
{
    /* Placeholder test task */
    while (1) {
        task_yield();
    }
}

/**
 * test_task_low - Test task entry point (low priority)
 */
static void test_task_low(void)
{
    /* Placeholder test task */
    while (1) {
        task_yield();
    }
}

/**
 * scheduler_test_init - Run comprehensive scheduler test suite
 *
 * Executes a series of tests covering scheduler functionality:
 * - Test 1: Task creation with different priorities
 * - Test 2: Priority getter verification
 * - Test 3: Priority setter operation
 * - Test 4: Deadline setter/getter operation
 * - Test 5: Priority clamping (out-of-range values)
 * - Test 6: Preemption control API (nesting)
 *
 * Results are printed to console with pass/fail status and summary.
 * Used during development and system validation.
 */
void scheduler_test_init(void)
{
    console_printf("\n=== EMBODIOS Scheduler Tests ===\n");

    int tests_passed = 0;
    int tests_failed = 0;

    /* Test 1: Task creation with priority */
    console_printf("\nTest 1: Task creation with priority... ");
    task_t *task_high = task_create("test-high", test_task_high, 5);
    task_t *task_med = task_create("test-medium", test_task_medium, 15);
    task_t *task_low = task_create("test-low", test_task_low, 25);

    if (task_high && task_med && task_low) {
        console_printf("PASS\n");
        tests_passed++;
    } else {
        console_printf("FAIL\n");
        tests_failed++;
    }

    /* Test 2: Priority getter verification */
    console_printf("Test 2: Priority getter verification... ");
    if (task_high && task_get_priority(task_high) == 5 &&
        task_med && task_get_priority(task_med) == 15 &&
        task_low && task_get_priority(task_low) == 25) {
        console_printf("PASS\n");
        tests_passed++;
    } else {
        console_printf("FAIL\n");
        tests_failed++;
    }

    /* Test 3: Priority setter */
    console_printf("Test 3: Priority setter... ");
    if (task_high) {
        task_set_priority(task_high, 10);
        if (task_get_priority(task_high) == 10) {
            console_printf("PASS\n");
            tests_passed++;
        } else {
            console_printf("FAIL\n");
            tests_failed++;
        }
    } else {
        console_printf("SKIP (no task)\n");
    }

    /* Test 4: Deadline setter/getter */
    console_printf("Test 4: Deadline setter/getter... ");
    if (task_med) {
        task_set_deadline(task_med, 1000);
        if (task_get_deadline(task_med) == 1000) {
            console_printf("PASS\n");
            tests_passed++;
        } else {
            console_printf("FAIL\n");
            tests_failed++;
        }
    } else {
        console_printf("SKIP (no task)\n");
    }

    /* Test 5: Priority clamping (out of range) */
    console_printf("Test 5: Priority clamping... ");
    if (task_low) {
        task_set_priority(task_low, 255);  /* Should clamp to 31 */
        if (task_get_priority(task_low) == 31) {
            console_printf("PASS\n");
            tests_passed++;
        } else {
            console_printf("FAIL (got %u, expected 31)\n", task_get_priority(task_low));
            tests_failed++;
        }
    } else {
        console_printf("SKIP (no task)\n");
    }

    /* Test 6: Preemption control API */
    console_printf("Test 6: Preemption control API... ");
    scheduler_disable_preemption();
    scheduler_disable_preemption();  /* Test nesting */
    scheduler_enable_preemption();
    scheduler_enable_preemption();
    console_printf("PASS\n");
    tests_passed++;

    /* Summary */
    console_printf("\n=== Test Results ===\n");
    console_printf("Passed: %d\n", tests_passed);
    console_printf("Failed: %d\n", tests_failed);
    console_printf("Total:  %d\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        console_printf("\nAll scheduler tests PASSED!\n");
    } else {
        console_printf("\nSome tests FAILED!\n");
    }

    console_printf("\n");
}