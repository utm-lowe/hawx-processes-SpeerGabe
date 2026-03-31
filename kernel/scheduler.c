#include "proc.h"
#include "riscv.h"

void scheduler(void) {
    struct proc *p;

    while(1) {
        // Loop over all processes
        for(int i = 0; i < NPROC; i++) {
            p = &proc[i];

            if(p->state != RUNNABLE)
                continue;

            // Mark process as running
            p->state = RUNNING;

            // Record that this CPU is running this process
            cpu.proc = p;

            // Context switch from scheduler to process
            swtch(&cpu.context, &p->context);

            // After process yields or terminates, we resume here
            cpu.proc = 0;

            // If process is done, its state is either WAITING or UNUSED
        }
    }
}

// Yield the CPU to the scheduler (called by processes)
void yield(void) {
    struct proc *p = cpu.proc;

    if(p == 0)
        return;

    // Mark current process as runnable again
    p->state = RUNNABLE;

    // Context switch back to scheduler
    swtch(&p->context, &cpu.context);
}