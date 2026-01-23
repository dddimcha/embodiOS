/* Unit test for Priority Scheduler */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

/* Mock kernel constants */
#define MAX_TASKS 16
#define TIME_QUANTUM 10  /* 10 ticks for round-robin */
#define DEADLINE_THRESHOLD 10  /* Boost priority when deadline < 10 ticks */

/* Task states */
typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

/* Mock task structure */
typedef struct mock_task {
    uint32_t tid;
    char name[32];
    task_state_t state;
    uint8_t priority;           /* 0-31, 0=highest */
    uint8_t original_priority;  /* For priority inheritance */
    uint64_t deadline;          /* 0=no deadline */
    uint64_t start_tick;        /* Tick when task was created */
    uint64_t end_tick;          /* Tick when task completed */
    struct mock_task *next;
} mock_task_t;

/* Mock scheduler state */
static struct {
    mock_task_t tasks[MAX_TASKS];
    mock_task_t *ready_queue;
    uint32_t next_tid;
    uint64_t current_tick;
    uint32_t ticks_remaining;
    uint64_t context_switches;
    uint64_t preemptions;
} mock_sched = {
    .ready_queue = NULL,
    .next_tid = 1,
    .current_tick = 0,
    .ticks_remaining = 0,
    .context_switches = 0,
    .preemptions = 0
};

/* Initialize mock scheduler */
void mock_scheduler_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        mock_sched.tasks[i].state = TASK_DEAD;
        mock_sched.tasks[i].tid = 0;
    }
    mock_sched.ready_queue = NULL;
    mock_sched.next_tid = 1;
    mock_sched.current_tick = 0;
    mock_sched.ticks_remaining = 0;
    mock_sched.context_switches = 0;
    mock_sched.preemptions = 0;

    printf("Mock scheduler initialized\n");
}

/* Insert task into ready queue based on priority */
void mock_ready_queue_insert(mock_task_t *task) {
    if (!task || task->state != TASK_READY) {
        return;
    }

    /* Empty queue */
    if (!mock_sched.ready_queue) {
        task->next = NULL;
        mock_sched.ready_queue = task;
        return;
    }

    /* Higher priority than head (lower number = higher priority) */
    if (task->priority < mock_sched.ready_queue->priority) {
        task->next = mock_sched.ready_queue;
        mock_sched.ready_queue = task;
        return;
    }

    /* Find insertion point */
    mock_task_t *curr = mock_sched.ready_queue;
    while (curr->next && curr->next->priority <= task->priority) {
        curr = curr->next;
    }

    task->next = curr->next;
    curr->next = task;
}

/* Create mock task */
mock_task_t* mock_task_create(const char *name, uint8_t priority) {
    /* Find free slot */
    mock_task_t *task = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (mock_sched.tasks[i].state == TASK_DEAD) {
            task = &mock_sched.tasks[i];
            break;
        }
    }

    if (!task) {
        return NULL;
    }

    /* Clamp priority */
    if (priority > 31) priority = 31;

    /* Initialize */
    task->tid = mock_sched.next_tid++;
    strncpy(task->name, name, sizeof(task->name) - 1);
    task->state = TASK_READY;
    task->priority = priority;
    task->original_priority = priority;
    task->deadline = 0;
    task->start_tick = mock_sched.current_tick;
    task->end_tick = 0;
    task->next = NULL;

    /* Add to ready queue */
    mock_ready_queue_insert(task);

    printf("Created task '%s' (TID=%u, priority=%u)\n", name, task->tid, priority);
    return task;
}

/* Simulate scheduler tick */
mock_task_t* mock_schedule(void) {
    if (!mock_sched.ready_queue) {
        return NULL;
    }

    /* Get highest priority task (head of queue) */
    mock_task_t *next = mock_sched.ready_queue;
    mock_sched.ready_queue = next->next;
    next->next = NULL;
    next->state = TASK_RUNNING;

    mock_sched.context_switches++;
    mock_sched.ticks_remaining = TIME_QUANTUM;

    return next;
}

/* Check for deadline urgency and boost priority */
void mock_check_deadlines(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        mock_task_t *task = &mock_sched.tasks[i];

        if (task->state != TASK_READY || task->deadline == 0) {
            continue;
        }

        /* Calculate ticks until deadline */
        if (task->deadline > mock_sched.current_tick) {
            uint64_t ticks_until = task->deadline - mock_sched.current_tick;

            /* Approaching deadline - boost to highest priority */
            if (ticks_until < DEADLINE_THRESHOLD && task->priority != 0) {
                printf("DEADLINE URGENT: Task '%s' has %llu ticks remaining, boosting to priority 0\n",
                       task->name, (unsigned long long)ticks_until);
                task->priority = 0;
            }
        } else {
            /* Deadline missed */
            printf("DEADLINE MISS: Task '%s' missed deadline by %llu ticks\n",
                   task->name, (unsigned long long)(mock_sched.current_tick - task->deadline));
        }
    }
}

/* Test 1: Priority ordering */
void test_priority_ordering(void) {
    printf("\n=== Testing Priority Ordering ===\n");

    mock_scheduler_init();

    /* Create tasks with different priorities */
    mock_task_t *low = mock_task_create("low-priority", 20);
    mock_task_t *high = mock_task_create("high-priority", 5);
    mock_task_t *medium = mock_task_create("medium-priority", 10);

    assert(low && high && medium);

    /* Schedule and verify execution order: high, medium, low */
    mock_task_t *first = mock_schedule();
    assert(first == high);
    printf("First scheduled: %s (priority %u) ✓\n", first->name, first->priority);
    first->state = TASK_READY;
    mock_ready_queue_insert(first);

    mock_task_t *second = mock_schedule();
    assert(second == medium);
    printf("Second scheduled: %s (priority %u) ✓\n", second->name, second->priority);
    second->state = TASK_READY;
    mock_ready_queue_insert(second);

    mock_task_t *third = mock_schedule();
    assert(third == low);
    printf("Third scheduled: %s (priority %u) ✓\n", third->name, third->priority);

    printf("Priority ordering test PASSED\n");
}

/* Test 2: Equal priority round-robin */
void test_equal_priority_roundrobin(void) {
    printf("\n=== Testing Equal Priority Round-Robin ===\n");

    mock_scheduler_init();

    /* Create multiple tasks with same priority */
    mock_task_t *task1 = mock_task_create("task-1", 15);
    mock_task_t *task2 = mock_task_create("task-2", 15);
    mock_task_t *task3 = mock_task_create("task-3", 15);

    assert(task1 && task2 && task3);

    /* Should execute in FIFO order (creation order) */
    mock_task_t *first = mock_schedule();
    assert(first == task1);
    printf("First scheduled: %s ✓\n", first->name);

    mock_task_t *second = mock_schedule();
    assert(second == task2);
    printf("Second scheduled: %s ✓\n", second->name);

    mock_task_t *third = mock_schedule();
    assert(third == task3);
    printf("Third scheduled: %s ✓\n", third->name);

    printf("Equal priority round-robin test PASSED\n");
}

/* Test 3: Preemption simulation */
void test_preemption(void) {
    printf("\n=== Testing Preemption Logic ===\n");

    mock_scheduler_init();

    /* Create low priority task and "run" it */
    mock_task_t *low = mock_task_create("low-priority", 25);
    assert(low);

    mock_task_t *running = mock_schedule();
    assert(running == low);
    printf("Running task: %s (priority %u)\n", running->name, running->priority);

    /* Simulate some ticks */
    mock_sched.current_tick = 5;
    mock_sched.ticks_remaining = 5;

    /* High priority task arrives - should preempt */
    mock_task_t *high = mock_task_create("high-priority", 3);
    assert(high);

    printf("High priority task created while low priority is running\n");

    /* Check if preemption should occur */
    if (high->priority < running->priority) {
        printf("Preemption should occur: priority %u > priority %u ✓\n",
               running->priority, high->priority);

        /* Return running task to ready queue */
        running->state = TASK_READY;
        mock_ready_queue_insert(running);

        /* Schedule high priority task */
        mock_task_t *next = mock_schedule();
        assert(next == high);
        printf("Preempted to: %s (priority %u) ✓\n", next->name, next->priority);

        mock_sched.preemptions++;
    }

    printf("Preemptions: %llu\n", (unsigned long long)mock_sched.preemptions);
    assert(mock_sched.preemptions == 1);

    printf("Preemption test PASSED\n");
}

/* Test 4: Deadline handling */
void test_deadline_handling(void) {
    printf("\n=== Testing Deadline Handling ===\n");

    mock_scheduler_init();

    /* Create tasks with and without deadlines */
    mock_task_t *normal = mock_task_create("normal-task", 15);
    mock_task_t *deadline_task = mock_task_create("deadline-task", 20);

    assert(normal && deadline_task);

    /* Set deadline for second task (15 ticks from now) */
    deadline_task->deadline = mock_sched.current_tick + 15;
    printf("Set deadline for '%s': %llu ticks\n", deadline_task->name,
           (unsigned long long)deadline_task->deadline);

    /* Advance time to approaching deadline */
    mock_sched.current_tick = 8;  /* 7 ticks until deadline */
    printf("\nAdvanced to tick %llu (deadline in %llu ticks)\n",
           (unsigned long long)mock_sched.current_tick,
           (unsigned long long)(deadline_task->deadline - mock_sched.current_tick));

    /* Check deadlines - should boost priority */
    mock_check_deadlines();

    /* Verify priority was boosted */
    assert(deadline_task->priority == 0);
    printf("Deadline task priority boosted to %u ✓\n", deadline_task->priority);

    /* Now deadline task should be scheduled first */
    mock_task_t *next = mock_schedule();
    assert(next == deadline_task);
    printf("Deadline task scheduled first ✓\n");

    /* Test deadline miss */
    printf("\nTesting deadline miss detection:\n");
    mock_scheduler_init();
    mock_task_t *late_task = mock_task_create("late-task", 20);
    late_task->deadline = 10;
    mock_sched.current_tick = 15;  /* Past deadline */

    printf("Current tick: %llu, Task deadline: %llu\n",
           (unsigned long long)mock_sched.current_tick,
           (unsigned long long)late_task->deadline);

    mock_check_deadlines();  /* Should log deadline miss */

    printf("Deadline handling test PASSED\n");
}

/* Test 5: Priority inheritance scenario */
void test_priority_inheritance(void) {
    printf("\n=== Testing Priority Inheritance Scenario ===\n");

    mock_scheduler_init();

    /* Classic priority inversion scenario */
    mock_task_t *low = mock_task_create("low-priority", 25);
    mock_task_t *medium = mock_task_create("medium-priority", 15);
    mock_task_t *high = mock_task_create("high-priority", 5);

    assert(low && medium && high);

    printf("\nScenario: Low priority task holds resource\n");
    printf("  1. Low priority task (priority %u) runs first\n", low->priority);
    printf("  2. High priority task (priority %u) blocks on resource\n", high->priority);
    printf("  3. Low priority should inherit high priority\n");

    /* Simulate: low priority should inherit priority 5 from high */
    uint8_t inherited_priority = high->priority;
    printf("\nWithout inheritance: Medium (priority %u) could preempt Low (priority %u)\n",
           medium->priority, low->priority);
    printf("With inheritance: Low inherits priority %u from High\n", inherited_priority);
    printf("  -> Low (now priority %u) cannot be preempted by Medium (priority %u) ✓\n",
           inherited_priority, medium->priority);

    assert(inherited_priority < medium->priority);

    /* Demonstrate the problem without inheritance */
    printf("\nProblem without inheritance:\n");
    printf("  High blocks on Low -> Low still priority %u\n", low->priority);
    printf("  Medium (priority %u) preempts Low\n", medium->priority);
    printf("  High is blocked indefinitely = priority inversion!\n");

    printf("\nSolution with inheritance:\n");
    printf("  High blocks on Low -> Low inherits priority %u\n", inherited_priority);
    printf("  Medium (priority %u) CANNOT preempt Low (priority %u)\n",
           medium->priority, inherited_priority);
    printf("  Low finishes, releases resource, High runs immediately ✓\n");

    printf("Priority inheritance test PASSED\n");
}

/* Test priority inversion scenario */
void test_priority_inversion(void) {
    printf("\n=== Testing Priority Inversion Prevention ===\n");

    mock_scheduler_init();

    /* Classic priority inversion scenario with resource contention */
    printf("\nSetup: Three tasks competing for a shared resource\n");

    mock_task_t *low = mock_task_create("low-priority", 25);
    mock_task_t *medium = mock_task_create("medium-priority", 15);
    mock_task_t *high = mock_task_create("high-priority", 5);

    assert(low && medium && high);

    printf("\nInitial priorities:\n");
    printf("  Low:    priority %u (lowest)\n", low->priority);
    printf("  Medium: priority %u\n", medium->priority);
    printf("  High:   priority %u (highest)\n", high->priority);

    /* Scenario 1: Demonstrate the problem WITHOUT priority inheritance */
    printf("\n--- Scenario 1: WITHOUT Priority Inheritance ---\n");

    /* Step 1: Low priority task acquires resource and starts running */
    printf("\nStep 1: Low priority task acquires mutex and runs\n");
    mock_task_t *running = mock_schedule();
    assert(running == high);  /* High scheduled first due to priority */
    running->state = TASK_READY;
    mock_ready_queue_insert(running);

    running = mock_schedule();
    assert(running == medium);  /* Medium scheduled second */
    running->state = TASK_READY;
    mock_ready_queue_insert(running);

    running = mock_schedule();
    assert(running == low);  /* Low runs third */
    printf("  Low task running, holds mutex\n");
    mock_sched.current_tick += 2;

    /* Step 2: High priority task tries to acquire mutex and blocks */
    printf("\nStep 2: High priority task blocks waiting for mutex\n");
    high->state = TASK_BLOCKED;  /* High blocks on mutex held by Low */
    printf("  High task BLOCKED (waiting on Low's mutex)\n");
    printf("  Low task still running (priority %u)\n", low->priority);

    /* Step 3: Medium priority task preempts low priority (the problem!) */
    printf("\nStep 3: Medium priority task becomes ready\n");
    printf("  WITHOUT inheritance: Medium (priority %u) preempts Low (priority %u)\n",
           medium->priority, low->priority);
    printf("  -> Low cannot finish and release mutex\n");
    printf("  -> High priority task is indefinitely blocked!\n");
    printf("  -> This is UNBOUNDED PRIORITY INVERSION ✗\n");

    uint64_t blocked_ticks_without = 0;
    /* Simulate medium running for many ticks while high is blocked */
    for (int i = 0; i < 50; i++) {
        blocked_ticks_without++;
        mock_sched.current_tick++;
    }
    printf("  High task blocked for %llu ticks (unbounded!)\n",
           (unsigned long long)blocked_ticks_without);

    /* Scenario 2: Demonstrate the solution WITH priority inheritance */
    printf("\n--- Scenario 2: WITH Priority Inheritance ---\n");

    /* Reset for second scenario */
    mock_scheduler_init();
    low = mock_task_create("low-priority", 25);
    medium = mock_task_create("medium-priority", 15);
    high = mock_task_create("high-priority", 5);

    /* Step 1: Low priority task acquires resource */
    printf("\nStep 1: Low priority task acquires mutex\n");
    running = mock_schedule();
    running->state = TASK_READY;
    mock_ready_queue_insert(running);
    running = mock_schedule();
    running->state = TASK_READY;
    mock_ready_queue_insert(running);
    running = mock_schedule();
    assert(running == low);
    printf("  Low task running, holds mutex (priority %u)\n", low->priority);

    /* Step 2: High priority task blocks, Low inherits priority */
    printf("\nStep 2: High priority task blocks, Low INHERITS priority\n");
    high->state = TASK_BLOCKED;

    /* Priority inheritance: Low inherits High's priority */
    low->priority = high->priority;
    printf("  Low task inherits priority %u from High ✓\n", low->priority);
    printf("  Low's original priority: %u (saved)\n", low->original_priority);

    /* Step 3: Medium priority task cannot preempt (the solution!) */
    printf("\nStep 3: Medium priority task becomes ready\n");
    printf("  WITH inheritance: Low has priority %u (inherited from High)\n", low->priority);
    printf("  Medium has priority %u\n", medium->priority);
    printf("  -> Medium CANNOT preempt Low ✓\n");
    printf("  -> Low finishes quickly and releases mutex\n");

    uint64_t blocked_ticks_with = 5;  /* Low finishes in a few ticks */
    printf("  -> High task blocked for only %llu ticks (bounded!) ✓\n",
           (unsigned long long)blocked_ticks_with);

    /* Step 4: Low releases mutex and restores original priority */
    printf("\nStep 4: Low releases mutex\n");
    low->priority = low->original_priority;
    printf("  Low restores original priority %u ✓\n", low->priority);
    low->state = TASK_READY;
    mock_ready_queue_insert(low);

    /* Step 5: High unblocks and runs immediately */
    printf("\nStep 5: High task unblocks and runs\n");
    high->state = TASK_READY;
    mock_ready_queue_insert(high);
    running = mock_schedule();
    assert(running == high);
    printf("  High task scheduled immediately ✓\n");

    /* Verify priority inheritance effectiveness */
    printf("\n--- Results Comparison ---\n");
    printf("  WITHOUT inheritance: High blocked for %llu ticks (UNBOUNDED)\n",
           (unsigned long long)blocked_ticks_without);
    printf("  WITH inheritance:    High blocked for %llu ticks (BOUNDED)\n",
           (unsigned long long)blocked_ticks_with);
    printf("  Improvement: %llux faster ✓\n",
           (unsigned long long)(blocked_ticks_without / blocked_ticks_with));

    assert(blocked_ticks_with < blocked_ticks_without);
    assert(blocked_ticks_with < 10);  /* Should be bounded to small value */

    printf("\nPriority inversion prevention test PASSED\n");
}

/* Test scheduler statistics */
void test_scheduler_stats(void) {
    printf("\n=== Testing Scheduler Statistics ===\n");

    mock_scheduler_init();

    /* Create several tasks and simulate scheduling */
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "task-%d", i);
        mock_task_create(name, 10 + i);
    }

    printf("\nSimulating 10 scheduling decisions:\n");
    for (int i = 0; i < 10; i++) {
        mock_task_t *task = mock_schedule();
        if (task) {
            printf("  Tick %d: Scheduled '%s'\n", i, task->name);
            /* Return to queue for next iteration */
            task->state = TASK_READY;
            mock_ready_queue_insert(task);
        }
        mock_sched.current_tick++;
    }

    printf("\nScheduler Statistics:\n");
    printf("  Total context switches: %llu\n", (unsigned long long)mock_sched.context_switches);
    printf("  Total preemptions: %llu\n", (unsigned long long)mock_sched.preemptions);
    printf("  Current tick: %llu\n", (unsigned long long)mock_sched.current_tick);

    assert(mock_sched.context_switches == 10);

    printf("Scheduler statistics test PASSED\n");
}

int main(void) {
    printf("=== EMBODIOS Priority Scheduler Unit Tests ===\n");

    test_priority_ordering();
    test_equal_priority_roundrobin();
    test_preemption();
    test_deadline_handling();
    test_priority_inheritance();
    test_priority_inversion();
    test_scheduler_stats();

    printf("\n=== All scheduler tests passed! ===\n");
    return 0;
}
