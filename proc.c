#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "uproc.h"

static char *states[] = {
[UNUSED]    "unused",
[EMBRYO]    "embryo",
[SLEEPING]  "sleep ",
[RUNNABLE]  "runble",
[RUNNING]   "run   ",
[ZOMBIE]    "zombie"
};

#ifdef CS333_P3
#define statecount NELEM(states)
#endif
#ifdef CS333_P4
const int PER_LINE = 15;  // per line max on print
static void assert(struct proc *p, enum procstate state, uint priority, const char * func, int line);
#endif



#ifdef CS333_P3
// record with head and tail pointer for constant-time access to the beginning
// and end of a linked list of struct procs.  use with stateListAdd() and
// stateListRemove().
struct ptrs {
  struct proc* head;
  struct proc* tail;
};
#endif

static struct {
  struct spinlock lock;
  struct proc proc[NPROC];
#ifdef CS333_P3
  struct ptrs list[statecount];
#endif
#ifdef CS333_P4
  struct ptrs ready[PRIO_MAX + 1];
  uint PromoteAtTime;
#endif
} ptable;

// list management function prototypes
#ifdef CS333_P3
static void initProcessLists(void);
static void initFreeList(void);
static void stateListAdd(struct ptrs*, struct proc*);
static int  stateListRemove(struct ptrs*, struct proc* p);
static void assertState(struct proc*, enum procstate, const char *, int);
#endif // CS333_P3
#ifdef CS333_P4
static void printReadyLists();
static void printReadyList(struct proc *, int);
#endif // CS333_P4

static struct proc *initproc;

uint nextpid = 1;
extern void forkret(void);
extern void trapret(void);
static void wakeup1(void* chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;

  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid) {
      return &cpus[i];
    }
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
#ifdef CS333_P3
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
  int flag =0;
  acquire(&ptable.lock);
  for(p = ptable.list[UNUSED].head; p != ptable.list[UNUSED].tail;p=p->next){
    if(p->state == UNUSED){
      flag=1;
      break;
    }
  }
  if(!flag){
    release(&ptable.lock);
    return 0;
  }
  if (stateListRemove(&ptable.list[UNUSED], p) == -1) {
    panic("ERROR in allocproc!");
  }
  assertState(p,UNUSED, __FUNCTION__, __LINE__);

  p->state = EMBRYO;
  stateListAdd(&ptable.list[EMBRYO], p);
  p->pid = nextpid++;
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    if (stateListRemove(&ptable.list[EMBRYO], p) == -1) {
      panic("ERROR in allocproc when allocating the kernel stack!");
    }

    assertState(p,EMBRYO , __FUNCTION__, __LINE__);
    p->state = UNUSED;
    stateListAdd(&ptable.list[UNUSED], p);
    release(&ptable.lock);
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  p->start_ticks=ticks;
  p->cpu_ticks_total = 0;
  p->cpu_ticks_in = 0;
  return p;
}
#else
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
  acquire(&ptable.lock);
  int found = 0;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED) {
      found = 1;
      break;
    }
  if (!found) {
    release(&ptable.lock);
    return 0;
  }
  p->state = EMBRYO;
  p->pid = nextpid++;
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  p->start_ticks=ticks;
  p->cpu_ticks_total = 0;
  p->cpu_ticks_in = 0;
  return p;
}
#endif

//PAGEBREAK: 32
// Set up first user process.
#ifdef CS333_P4
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  acquire(&ptable.lock);

  initProcessLists();
  initFreeList();
  ptable.PromoteAtTime = ticks + TICKS_TO_PROMOTE;

  release(&ptable.lock);
  p = allocproc();
  acquire(&ptable.lock);

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S
  safestrcpy(p->name, "initcode", sizeof(p->name));

  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  int rc= getpriority(p->pid);
  if (stateListRemove(&ptable.list[EMBRYO], p) == -1)
    panic("State List Remove not successful in userinit");
  assertState(p, EMBRYO, __FUNCTION__, __LINE__);
  p->state = RUNNABLE;
  stateListAdd(&ptable.ready[rc], p);
  release(&ptable.lock);
}
#elif CS333_P3
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  //initialize lists here
  acquire(&ptable.lock);
  initProcessLists();
  initFreeList();
  release(&ptable.lock);

  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  if (stateListRemove(&ptable.list[EMBRYO], p) == -1) {
    panic("ERROR in userinit!");
  }
  assertState(p, EMBRYO, __FUNCTION__, __LINE__);

  p->state = RUNNABLE;
  stateListAdd(&ptable.list[RUNNABLE], p);
  release(&ptable.lock);
}
#else
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  //initialize lists here
  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}
#endif

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
#ifdef CS333_P4
int
fork(void)
{
  int i;
  uint pid;
  struct proc *np;
  struct proc *curproc = myproc();


  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  acquire(&ptable.lock);

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    if (stateListRemove(&ptable.list[EMBRYO], np) == -1) { 
      panic("Failed to remove Embryo from list in Fork. Can't transition to Unused");
    }
    assertState(np, EMBRYO, __FUNCTION__, __LINE__);
    np->state = UNUSED;
    stateListAdd(&ptable.list[UNUSED], np);
    release(&ptable.lock);
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;
  int rc= getpriority(np->pid);

  if (stateListRemove(&ptable.list[EMBRYO], np) == -1) { 
    panic("Failed to remove Embryo from list in Fork. Can't transition to Unused");
  }
  assertState(np, EMBRYO, __FUNCTION__, __LINE__);
  np->state = RUNNABLE;
  stateListAdd(&ptable.ready[rc], np);
  release(&ptable.lock);

  return pid;
}

#elif CS333_P3
int
fork(void)
{
  int i;
  uint pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    acquire(&ptable.lock);
    if (stateListRemove(&ptable.list[EMBRYO], np) == -1) {
      panic("ERROR in fork");
    }
    assertState(np, EMBRYO, __FUNCTION__, __LINE__);
    np->state = UNUSED;

    stateListAdd(&ptable.list[UNUSED],np);
    release(&ptable.lock);
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
  np->uid=curproc->uid;
  np->gid=curproc->gid;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);
  if (stateListRemove(&ptable.list[EMBRYO], np) == -1) {
    panic("TODO write a helpful error message here!");
  }
  assertState(np, EMBRYO, __FUNCTION__, __LINE__);
  np->state = RUNNABLE;
  stateListAdd(&ptable.list[RUNNABLE], np);
  release(&ptable.lock);
  
  return pid;
}
#else
int
fork(void)
{
  int i;
  uint pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
   return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
  np->uid=curproc->uid;
  np->gid=curproc->gid;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);
    
  
  return pid;
}
#endif

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
#ifdef CS333_P4
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (int i =0; i <= NELEM(states); i++) {
    p = ptable.list[i].head;
    if(p != NULL){
      if(p->parent == curproc){
        p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
      }
      p=p->next;
    }
  }
  // Jump into the scheduler, never to return.
  if (stateListRemove(&ptable.list[RUNNING], curproc) == -1) {
    panic("ERROR in exit");
  }
  assertState(curproc, RUNNING, __FUNCTION__, __LINE__);
  curproc->state = ZOMBIE;
  stateListAdd(&ptable.list[ZOMBIE], curproc);
#ifdef PDX_XV6
  curproc->sz = 0;
#endif // PDX_XV6
  sched();
  panic("zombie exit");
}
#elif CS333_P3
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (int i =0; i <= NELEM(states); i++) {
    p = ptable.list[i].head;
    if(p != NULL){
      if(p->parent == curproc){
        p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
      }
      p=p->next;
    }
  }
  // Jump into the scheduler, never to return.
  if (stateListRemove(&ptable.list[RUNNING], curproc) == -1) {
    panic("ERROR in exit");
  }
  assertState(curproc, RUNNING, __FUNCTION__, __LINE__);
  curproc->state = ZOMBIE;
  stateListAdd(&ptable.list[ZOMBIE], curproc);
#ifdef PDX_XV6
  curproc->sz = 0;
#endif // PDX_XV6
  sched();
  panic("zombie exit");
}
#else
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
#ifdef PDX_XV6
  curproc->sz = 0;
#endif // PDX_XV6
  sched();
  panic("zombie exit");
}
#endif

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
#ifdef CS333_P4
int
wait(void)
{
  struct proc *p;
  int havekids;
  uint pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(int i = EMBRYO; i <= ZOMBIE; i++){
      if(i == RUNNABLE)
      {
        for(int i = 0; i <= PRIO_MAX; ++i)
        {
          for(p = ptable.ready[i].head; p != NULL; p = p->next)
          {
            if(p->parent != curproc)
              continue;
            havekids = 1;
          }
        }
      }
      else
      {
      for(p = ptable.list[i].head; p != NULL; p = p->next){
        if(p->parent != curproc)
          continue;
        havekids = 1;
        if(p->state == ZOMBIE){
          // Found one.
          pid = p->pid;
          kfree(p->kstack);
          p->kstack = 0;
          freevm(p->pgdir);
          p->pid = 0;
          p->parent = 0;
          p->name[0] = 0;
          p->killed = 0;
          if (stateListRemove(&ptable.list[ZOMBIE], p) == -1) { 
            panic("Zombie state wasn't removed in wait");
          }
          assertState(p, ZOMBIE, __FUNCTION__, __LINE__);
          p->state = UNUSED;
          stateListAdd(&ptable.list[UNUSED], p);
          release(&ptable.lock);
          return pid;
        }
      }
    }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}
#elif CS333_P3
int
wait(void)
{
  struct proc *p;
  int havekids;
  uint pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
  for (int i = UNUSED+1; i < NELEM(states); i++) {
    p = ptable.list[i].head;
    while(p != NULL){
      if(p->parent != curproc){
        p=p->next;
        continue;
      }
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        if (stateListRemove(&ptable.list[ZOMBIE], p) == -1) {
          panic("ERROR in wait!");
        }
        assertState(p, ZOMBIE, __FUNCTION__, __LINE__);
        p->state = UNUSED;
        stateListAdd(&ptable.list[UNUSED], p);
        release(&ptable.lock);
        return pid;
      }
      p=p->next;
    }
  }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}
#else
int
wait(void)
{
  struct proc *p;
  int havekids;
  uint pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}
#endif

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
#ifdef CS333_P4
void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
#ifdef PDX_XV6
  int idle;  // for checking if processor is idle
#endif // PDX_XV6

  for(;;){
    // Enable interrupts on this processor.
    sti();

#ifdef PDX_XV6
    idle = 1;  // assume idle unless we schedule a process
#endif // PDX_XV6
    acquire(&ptable.lock);
    // Loop over process table looking for process to run.
    struct proc* lists;
    for(int i = PRIO_MAX; i >= 0; --i)
    {
      lists = ptable.ready[i].head;
      if(lists)
        break;
    }
    if(lists != NULL)
    {
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
#ifdef PDX_XV6
      idle = 0;  // not idle this timeslice
#endif // PDX_XV6
      c->proc = lists;
      switchuvm(lists);

      int rc = getpriority(lists->pid);
      if(rc == -1)
      {
        cprintf("Schedule");
        panic("Couldn't find PID in getpriority in scheduler");
      }
      if(rc < 0 || rc > PRIO_MAX)
        panic("Priority was too large can't add to ready list.");
      if(stateListRemove(&ptable.ready[rc], lists) == -1){
        panic("Removal Runnable process in ready list in scheduler failed.");
      }
      assert(lists, RUNNABLE, rc, __FUNCTION__, __LINE__);
      lists->state = RUNNING;
      stateListAdd(&ptable.list[RUNNING], lists);

      swtch(&(c->scheduler), lists->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      if(ptable.PromoteAtTime <= ticks)
      {
        struct proc *p;
        int d;
        for(int i = 0; i <= PRIO_MAX; ++i)
        {
          for(p = ptable.ready[i].head; p != NULL; p = p->next)
          {
            d = getpriority(p->pid);
            if(d == -1)
              panic("Couldn't find PID in getpriority during promotion");
            if(d == PRIO_MAX)
            {
              if(setpriority(p->pid, d) == -1)
                panic("setpriority failed during promotion ready");
            }
            else
            {
              if(setpriority(p->pid, d+1) == -1)
                panic("setpriority failed during promotion reday not max");
            }
          }
        }

        for(p = ptable.list[SLEEPING].head; p != NULL; p = p->next){
          d = getpriority(p->pid);
          if(d == -1)
            panic("Couldn't find PID in getpriority during promotion");
          if(d == PRIO_MAX)
          {
            if(setpriority(p->pid, d) == -1)
              panic("setpriority failed during promotion sleeping");
          }
          else
          {
            if(setpriority(p->pid, d+1) == -1)
              panic("setpriority failed during promotion sleeping not max");
          }
        }

        for(p = ptable.list[RUNNING].head; p != NULL; p = p->next){
          d = getpriority(p->pid);
          if(d == -1)
            panic("Couldn't find PID in getpriority during promotion");
          if(d == PRIO_MAX)
          {
            if(setpriority(p->pid, d) == -1)
              panic("setpriority failed during promotion running");
          }
          else
          {
            if(setpriority(p->pid, d+1) == -1)
              panic("setpriority failed during promotion running not max");
          }
        }
        ptable.PromoteAtTime = ticks + TICKS_TO_PROMOTE;
      }
    }
    release(&ptable.lock);
#ifdef PDX_XV6
    // if idle, wait for next interrupt
    if (idle) {
      sti();
      hlt();
    }
#endif // PDX_XV6
  }  
}
#elif CS333_P3
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
#ifdef PDX_XV6
  int idle;  // for checking if processor is idle
#endif // PDX_XV6

  for(;;){
    // Enable interrupts on this processor.
    sti();

#ifdef PDX_XV6
    idle = 1;  // assume idle unless we schedule a process
#endif // PDX_XV6
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    p=ptable.list[RUNNABLE].head;
    if(p != NULL){
      if(stateListRemove(&ptable.list[RUNNABLE], p) == -1){
        panic("ERROR in scheduler!");
      }
      assertState(p, RUNNABLE, __FUNCTION__, __LINE__);
      p->state = RUNNING;
      stateListAdd(&ptable.list[RUNNING], p);
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
#ifdef PDX_XV6
      idle = 0;  // not idle this timeslice
#endif // PDX_XV6
      c->proc = p;
      switchuvm(p);

      p->cpu_ticks_in=ticks;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
#ifdef PDX_XV6
    // if idle, wait for next interrupt
    if (idle) {
      sti();
      hlt();
    }
#endif // PDX_XV6
  }
}
#else
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
#ifdef PDX_XV6
  int idle;  // for checking if processor is idle
#endif // PDX_XV6

  for(;;){
    // Enable interrupts on this processor.
    sti();

#ifdef PDX_XV6
    idle = 1;  // assume idle unless we schedule a process
#endif // PDX_XV6
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
#ifdef PDX_XV6
      idle = 0;  // not idle this timeslice
#endif // PDX_XV6
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      p->cpu_ticks_in=ticks;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
#ifdef PDX_XV6
    // if idle, wait for next interrupt
    if (idle) {
      sti();
      hlt();
    }
#endif // PDX_XV6
  }
}
#endif

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();
  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  p->cpu_ticks_total += (ticks - p->cpu_ticks_in);
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
#ifdef CS333_P4
void
yield(void)
{
 struct proc *curproc = myproc();

  acquire(&ptable.lock);  //DOC: yieldlock
  int rc = getpriority(curproc->pid);
  if (stateListRemove(&ptable.list[RUNNING], curproc) == -1) { 
    panic("Running state wasn't removed in yield");
  }
  assertState(curproc, RUNNING, __FUNCTION__, __LINE__);
  curproc->state = RUNNABLE;

  if(rc == -1)
    panic("Couldn't find PID in getpriority");
  if(rc < 0 || rc > PRIO_MAX)
    panic("Priority was too large can't add to ready list.");
  stateListAdd(&ptable.ready[rc], curproc);
  curproc->budget = curproc->budget - (ticks - curproc->cpu_ticks_in);
  if(curproc->budget <= 0)
  {
    int prio = getpriority(curproc->pid);
    if(prio == -1)
      panic("getpriority failed in yield");
    if(prio == 0){
      if(setpriority(curproc->pid, prio) == -1)
        panic("Setpriority failed in yeild min demotion");
    }
    else{
      if(setpriority(curproc->pid, prio-1) == -1)
        panic("Setpriority failed in yield");
    }
  }
  sched();
  release(&ptable.lock);
}  
#elif CS333_P3
void
yield(void)
{
  struct proc *curproc = myproc();

  acquire(&ptable.lock);  //DOC: yieldlock
  if(stateListRemove(&ptable.list[RUNNING], curproc) == -1) 
    panic("ERROR in yield!");
  assertState(curproc, RUNNING, __FUNCTION__, __LINE__);
  curproc->state = RUNNABLE;
  stateListAdd(&ptable.list[RUNNABLE], curproc);
  sched();
  release(&ptable.lock);
}
#else
void
yield(void)
{
  struct proc *curproc = myproc();

  acquire(&ptable.lock);  //DOC: yieldlock
  curproc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}
#endif

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
#ifdef CS333_P4
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    if (lk) release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  if (stateListRemove(&ptable.list[RUNNING], p) == -1) { 
    panic("Running state wasn't removed in sleep");
  }
  assertState(p, RUNNING, __FUNCTION__, __LINE__);
  p->state = SLEEPING;
  stateListAdd(&ptable.list[SLEEPING], p);
  p->budget = p->budget - (ticks - p->cpu_ticks_in);
  if(p->budget <= 0)
  {
    int prio = getpriority(p->pid);
    if(prio == -1)
      panic("getpriority failed in sleep");
    if(prio == 0){
      if(setpriority(p->pid, prio) == -1)
        panic("Setpriority failed in sleep when at lowest priority");
    }
    else{
      //cprintf("\nState %s\n", p->state);
      if(setpriority(p->pid, prio-1) == -1)
        panic("Setpriority failed in sleep");
    }
  }

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    if (lk) acquire(lk);
  }
}
#elif CS333_P3
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    if (lk) release(lk);
  }

  // Go to sleep.
  p->chan = chan;
  if (stateListRemove(&ptable.list[RUNNING], p) == -1) {
    panic("ERROR in sleep!");
  }
  assertState(p, RUNNING, __FUNCTION__, __LINE__);
  p->state = SLEEPING;
  stateListAdd(&ptable.list[SLEEPING], p);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    if (lk) acquire(lk);
  }
}
#else
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    if (lk) release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    if (lk) acquire(lk);
  }
}
#endif 

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
#ifdef CS333_P4
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.list[SLEEPING].head; p != NULL; p = p->next)
  {
    if(p->state == SLEEPING && p->chan == chan)
    {
      int rc = getpriority(p->pid);
      if (stateListRemove(&ptable.list[SLEEPING], p) == -1) { 
        panic("Sleeping state wasn't removed in wakeup1");
      }
      assertState(p, SLEEPING, __FUNCTION__, __LINE__);
      p->state = RUNNABLE;
      if(rc == -1)
        panic("Couldn't find PID in getpriority");
      if(rc < 0 || rc > PRIO_MAX)
        panic("Priority was too large can't add to ready list.");
      stateListAdd(&ptable.ready[rc], p);
    }
  }
}
#elif CS333_P3
static void
wakeup1(void *chan)
{
  struct proc *p;
 struct proc *temp; 
  p=ptable.list[SLEEPING].head;
  while(p != NULL){
    if(p->chan == chan){
      temp=p->next;
      if (stateListRemove(&ptable.list[SLEEPING], p) == -1) {
        panic("ERROR in wakeup1!");
      }
      assertState(p, SLEEPING, __FUNCTION__, __LINE__);
      p->state = RUNNABLE;
      stateListAdd(&ptable.list[RUNNABLE], p);
      p=temp;
    }
    else
      p=p->next;
  }
}
#else
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}
#endif

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
#ifdef CS333_P4
int
kill(int pid)
{
struct proc *p;

  acquire(&ptable.lock);
  for(int i = EMBRYO; i <= ZOMBIE; i++){
    if(i == RUNNABLE)
    {
      for(int i = 0; i <= PRIO_MAX; ++i)
      {
        for(p = ptable.ready[i].head; p != NULL; p = p->next)
        {
          if(p->pid == pid){
            p->killed = 1;
            release(&ptable.lock);
            return 0;
          }
        }
      }
    }
    else
    {
    for(p = ptable.list[i].head; p != NULL; p = p->next)
    {
      if(p->pid == pid){
        p->killed = 1;
        // Wake process from sleep if necessary.
        if(p->state == SLEEPING)
        {
          int rc = getpriority(p->pid);
          if (stateListRemove(&ptable.list[SLEEPING], p) == -1) { 
            panic("Sleeping state wasn't removed in kill");
          }
          assertState(p, SLEEPING, __FUNCTION__, __LINE__);
          p->state = RUNNABLE;
          if(rc == -1)
            panic("Couldn't find PID in getpriority");
          if(rc < 0 || rc > PRIO_MAX)
            panic("Priority was too large can't add to ready list.");
          stateListAdd(&ptable.ready[rc], p);
        }
        release(&ptable.lock);
        return 0;
      }
    }
    }
  }
  release(&ptable.lock);
  return -1;
}
#elif CS333_P3
int
kill(int pid)
{
  struct proc *p;
  struct proc *temp;
  acquire(&ptable.lock);
  for (int i = 1; i < NELEM(states); i++) {
    p=ptable.list[i].head;
    while(p != NULL){
      if(p->pid == pid){
        p->killed = 1;
        // Wake process from sleep if necessary.
        if(p->state==SLEEPING){
          temp=p->next;
          if (stateListRemove(&ptable.list[SLEEPING], p) == -1) {
            panic("ERROR in kill!");
          }
          assertState(p, SLEEPING, __FUNCTION__, __LINE__);
          p->state = RUNNABLE;
          stateListAdd(&ptable.list[RUNNABLE], p);
          p=temp;
        }
        release(&ptable.lock);
        return 0;
      }
      p=p->next;
    }
  }
  release(&ptable.lock);
  return -1;
}
#else
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}
#endif

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
/*#if defined(CS333_P4)
void
procdumpP4(struct proc *p, char *state_string)
{
  struct proc *parent;
  uint seconds = (ticks - p->start_ticks) / 1000;
  uint milliseconds = (ticks - p->start_ticks) % 1000;
  cprintf("%d\t%s\t     %d\t\t%d", p->pid, p->name,  p->uid, p->gid);
  parent = p->parent;
  if(parent == NULL)
    cprintf("\t%d\t%d", p->pid, p->priority);
  else
    cprintf("\t%d\t%d", parent->pid, p->priority);

  if(milliseconds <= 9)
    cprintf("\t%d.00%d", seconds, milliseconds);
  else if(milliseconds <= 99)
    cprintf("\t%d.0%d", seconds, milliseconds);
  else
    cprintf("\t%d.%d", seconds, milliseconds);

  seconds = p->cpu_ticks_total / 1000;
  milliseconds = p->cpu_ticks_total % 1000;
  if(milliseconds <= 9)
    cprintf("\t%d.00%d", seconds, milliseconds);
  else if(milliseconds <= 99)
    cprintf("\t%d.0%d", seconds, milliseconds);
  else
    cprintf("\t%d.%d", seconds, milliseconds);

  cprintf("\t%s\t%d   ", state_string, p->sz);
  return;
}
#endif*/
#if defined(CS333_P4)
void
procdumpP2P3P4(struct proc *p, char *state_string)
{
  uint num=(ticks-p->start_ticks);
  uint milli=num % 1000;
  num=(num/1000) % 60;
  uint cpu = p->cpu_ticks_total;
  p->cpu_ticks_total = p->cpu_ticks_total % 1000;
  cpu=(cpu/1000) % 60;
  cprintf("%d\t%s\t     %d\t\t%d\t%d\t%d\t%d.%d\t%d.%d\t%s\t%d  ",p->pid,p->name,p->uid,p->gid,p->pid,p->priority,num,milli,cpu,p->cpu_ticks_total,states[p->state],p->sz);
  return;
}
#elif defined(CS333_P1)
void
procdumpP1(struct proc *p, char *state_string)
{
  uint num=(ticks-p->start_ticks);
  uint milli=num % 1000;
  num=(num/1000) % 60;
  cprintf("%d \t%s \t      %d.%d \t%s \t%d   ",p->pid,p->name,num,milli,states[p->state],p->sz);
  return;
}
#endif

void
procdump(void)
{
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

#if defined(CS333_P4)
#define HEADER "\nPID\tName         UID\tGID\tPPID\tPrio\tElapsed\tCPU\tState\tSize\t PCs\n"
#elif defined(CS333_P2)
#define HEADER "\nPID\tName         UID\tGID\tPPID\tElapsed\tCPU\tState\tSize\t PCs\n"
#elif defined(CS333_P1)
#define HEADER "\nPID\tName         Elapsed\tState\tSize\t PCs\n"
#else
#define HEADER "\n"
#endif

  cprintf(HEADER);  // not conditionally compiled as must work in all project states

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    // see TODOs above this function
    // P2 and P3 are identical and the P4 change is minor
#if defined(CS333_P2)
    procdumpP2P3P4(p, state);
#elif defined(CS333_P1)
    procdumpP1(p, state);
#else
    cprintf("%d\t%s\t%s\t", p->pid, p->name, state);
#endif

    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
#ifdef CS333_P1
  cprintf("$ ");  // simulate shell prompt
#endif // CS333_P1
}

#if defined(CS333_P3)
// list management helper functions
static void
stateListAdd(struct ptrs* list, struct proc* p)
{
  if((*list).head == NULL){
    (*list).head = p;
    (*list).tail = p;
    p->next = NULL;
  } else{
    ((*list).tail)->next = p;
    (*list).tail = ((*list).tail)->next;
    ((*list).tail)->next = NULL;
  }
}
#endif

#if defined(CS333_P3)
static int
stateListRemove(struct ptrs* list, struct proc* p)
{
  if((*list).head == NULL || (*list).tail == NULL || p == NULL){
    return -1;
  }

  struct proc* current = (*list).head;
  struct proc* previous = 0;

  if(current == p){
    (*list).head = ((*list).head)->next;
    // prevent tail remaining assigned when we've removed the only item
    // on the list
    if((*list).tail == p){
      (*list).tail = NULL;
    }
    return 0;
  }

  while(current){
    if(current == p){
      break;
    }

    previous = current;
    current = current->next;
  }

  // Process not found. return error
  if(current == NULL){
    return -1;
  }

  // Process found.
  if(current == (*list).tail){
    (*list).tail = previous;
    ((*list).tail)->next = NULL;
  } else{
    previous->next = current->next;
  }

  // Make sure p->next doesn't point into the list.
  p->next = NULL;

  return 0;
}
#endif

#if defined(CS333_P3)
static void
initProcessLists()
{
  int i;

  for (i = UNUSED; i <= ZOMBIE; i++) {
    ptable.list[i].head = NULL;
    ptable.list[i].tail = NULL;
  }
#if defined(CS333_P4)
  for (i = 0; i <= PRIO_MAX; i++) {
    ptable.ready[i].head = NULL;
    ptable.ready[i].tail = NULL;
  }
#endif
}
#endif

#if defined(CS333_P3)
static void
initFreeList(void)
{
  struct proc* p;

  for(p = ptable.proc; p < ptable.proc + NPROC; ++p){
    p->state = UNUSED;
    stateListAdd(&ptable.list[UNUSED], p);
  }
}
#endif
#ifdef CS333_P4
static void
assert(struct proc *p, enum procstate state, uint priority, const char * func, int line)
{
    if (p->state != state)
    {
      cprintf("Error: proc state is %s and should be %s.\nCalled from %s line %d\n",
        states[p->state], states[state], func, line);
      panic("Error: Process state incorrect in assertState()");
    }
    if(p->priority != priority)
    {
      cprintf("Error: proc priority is %d and should be %d.\nCalled from %s line %d\n",
        p->priority, priority, func, line);
      panic("Error: Process state incorrect in assertState()");
    }
    return;
}
#endif
#if defined(CS333_P3)
// example usage:
// assertState(p, UNUSED, __FUNCTION__, __LINE__);
// This code uses gcc preprocessor directives. For details, see
// https://gcc.gnu.org/onlinedocs/cpp/Standard-Predefined-Macros.html
static void
assertState(struct proc *p, enum procstate state, const char * func, int line)
{
    if (p->state == state)
      return;
    cprintf("Error: proc state is %s and should be %s.\nCalled from %s line %d\n",
        states[p->state], states[state], func, line);
    panic("Error: Process state incorrect in assertState()");
}
#endif

#if defined(CS333_P3)
// Project 3/4 control sequence support
void
printList(int state)
{
  int count = 0;
  const int PER_LINE = 15;  // per line max on print
  const int PER_LINE_Z = (PER_LINE/2);  // zombie list has more chars per entry on print
  struct proc *p;
  static char *stateNames[] = {  // note: sparse array
    [RUNNABLE]  "Runnable",
    [SLEEPING]  "Sleep",
    [RUNNING]   "Running",
    [ZOMBIE]    "Zombie"
  };


  if (state < UNUSED || state > ZOMBIE) {
    cprintf("Invalid control sequence\n");
    cprintf("$ ");  // simulate shell prompt
    return;
  }

  acquire(&ptable.lock);
#ifdef DEBUG
  checkProcs(__FILE__, __FUNCTION__, __LINE__);
#endif
#ifdef CS333_P4
  if (state == RUNNABLE) {
    printReadyLists();
    release(&ptable.lock);
    cprintf("$ ");  // simulate shell prompt
    return;
  }
#endif
  cprintf("\n%s List Processes:\n", stateNames[state]);
  p = ptable.list[state].head;
  while (p != NULL) {
    if (p->state != state) {  // sanity check
      cprintf("Error: PID %d on %s list but should be on %s\n",
          p->pid, states[p->state], states[state]);
      panic("Corrupted list\n");
    }
    if (state == ZOMBIE)
      cprintf("(%d, %d)", p->pid,
          (p->parent) ? p->parent->pid : p->pid);
    else
      cprintf("%d", p->pid);
    p = p->next;
    cprintf("%s", p ? " -> " : "\n");
    if (p && (++count) %
        ((state == ZOMBIE) ? PER_LINE_Z : PER_LINE) == 0)
      cprintf("\n");
  }
  release(&ptable.lock);
  cprintf("$ ");  // simulate shell prompt
  return;
}

void
printFreeList(void)
{
  int count = 0;
  struct proc *p;

  acquire(&ptable.lock);
  p = ptable.list[UNUSED].head;
  while (p != NULL) {
    count++;
    p = p->next;
  }
  release(&ptable.lock);
  cprintf("\nFree List Size: %d processes\n", count);
  cprintf("$ ");  // simulate shell prompt
  return;
}

void
printListStats()
{
  int i, count, total = 0;
  struct proc *p;

  acquire(&ptable.lock);
  for (i=UNUSED; i<=ZOMBIE; i++) {
    count = 0;
    p = ptable.list[i].head;
    while (p != NULL) {
      count++;
      if(p->state != i) {
        cprintf("\nlist invariant failed: process %d has state %s but is on list %s\n",
            p->pid, states[p->state], states[i]);
      }
      p = p->next;
    }
    cprintf("\n%s list has ", states[i]);
    if (count < 10) cprintf(" ");  // line up columns. we know NPROC < 100
    cprintf("%d processes", count);
    total += count;
  }
  release(&ptable.lock);
  cprintf("\nTotal on lists is: %d. NPROC = %d. %s",
      total, NPROC, (total == NPROC) ? "Congratulations!" : "Bummer");
  cprintf("\n$ ");  // simulate shell prompt
  return;
}
#endif // CS333_P3

#ifdef CS333_P4
void
printReadyList(struct proc *p, int prio)
{
  if (p == NULL) {
    cprintf("(NULL)\n");
    return;
  }
  int count = 0;
  do {
    cprintf("pid: %d, Budget: %d", p->pid,p->budget);
    if(p->priority != prio) {
      cprintf("\nlist invariant failed: process %d has prio %d but is on runnable list %d\n",
          p->pid, p->priority, prio);
    }
    p = p->next;
    cprintf("%s", p ? " -> " : "\n");
    if (p && (++count) % PER_LINE == 0)
      cprintf("\n");
  } while (p != NULL);
}

void
printReadyLists()
{
  struct proc *p;

  cprintf("Ready List Processes:\n");
// this look must be changed based on MAX/MIN prio
  for (int i=PRIO_MAX; i>=PRIO_MIN; i--) {
    p = ptable.ready[i].head;
    cprintf("Prio %d: ", i);
    if(p->state != RUNNABLE) {
      cprintf("\nlist invariant failed: process %d has state %s but is on ready list\n",
          p->pid, states[p->state]);
    }
    printReadyList(p, i);
  }
}

#endif // CS333_P4

#ifdef CS333_P2
int
getprocshelper(uint max,struct uproc * table)
{
  struct proc * p;
  int num=0;//number of tables copied
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC] && num < max; p++){
    if(p->state != UNUSED && p->state != EMBRYO){
      table->pid = p->pid;
      safestrcpy(table->name,p->name,STRMAX);
      table->uid=p->uid;
      table->gid=p->gid;
      if(p->parent == NULL)
        table->ppid=p->pid;
      else
        table->ppid=p->parent->pid;
      table->elapsed_ticks=ticks - p->start_ticks;
      table->CPU_total_ticks=p->cpu_ticks_total;
      safestrcpy(table->state,states[p->state],STRMAX);
      table->size=p->sz;
      ++table;
      ++num;
    }
  }
  release(&ptable.lock);
  return num;
}
#endif
 
#ifdef CS333_P4
int 
setpriority( int pid,int priority)
{
  if(priority < 0 || priority > PRIO_MAX)
    return -1;

  struct proc *p;
  for(int i = 0; i <= PRIO_MAX; ++i)
  {
    for(p = ptable.ready[i].head; p != NULL; p = p->next)
    {
      if(p->pid == pid)
      {
        if(priority == p->priority)
        {
          p->budget = DEFAULT_BUDGET;
          return priority;
        }
        if (stateListRemove(&ptable.ready[i], p) == -1) {
          panic("Process incorrectly removed when setting priority. panic");
        }
        assert(p, RUNNABLE, i, __FUNCTION__, __LINE__);
        p->priority = priority;
        p->budget = DEFAULT_BUDGET;
        stateListAdd(&ptable.ready[priority], p);
        return priority;
      }
    }
  }

  for(p = ptable.list[SLEEPING].head; p != NULL; p = p->next){
    if(p->pid == pid)
    {
      if(priority == p->priority)
      {
        p->budget = DEFAULT_BUDGET;
        return priority;
      }
      p->priority = priority;
      p->budget = DEFAULT_BUDGET;
      return priority;
    }
  }

  for(p = ptable.list[RUNNING].head; p != NULL; p = p->next){
    if(p->pid == pid)
    {
      if(priority == p->priority)
      {
        p->budget = DEFAULT_BUDGET;
        return priority;
      }
      p->priority = priority;
      p->budget = DEFAULT_BUDGET;
      return priority;
    }
  }

  cprintf("didn't find");
  return -1;
}

int 
getpriority(int pid)
{
  if(pid < 0)
    return -1;
  struct proc *p;
  for(int i = 0; i <= PRIO_MAX; ++i)
  {
    for(p = ptable.ready[i].head; p != NULL; p = p->next)
    {
      if(p->pid == pid)
      {
          return p->priority;
      }
    }
  }

  for(int i = EMBRYO; i <= ZOMBIE; i++){
    for(p = ptable.list[i].head; p != NULL; p = p->next){
      if(p->pid == pid)
      {
        return p->priority;
      }
    }
  }
  cprintf("didn't find\n");
  return -1;
}
#endif


/*
  int i;
  uint pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    acquire(&ptable.lock);
    if (stateListRemove(&ptable.list[EMBRYO], np) == -1) {
      panic("ERROR in fork");
    }
    assertState(np, EMBRYO, __FUNCTION__, __LINE__);
    np->state = UNUSED;

    stateListAdd(&ptable.list[UNUSED],np);
    release(&ptable.lock);
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
  np->uid=curproc->uid;
  np->gid=curproc->gid;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  int rc = getpriority(np->pid);

  acquire(&ptable.lock);
  if (stateListRemove(&ptable.list[EMBRYO], np) == -1) {
    panic("TODO write a helpful error message here!");
  }
  assertState(np, EMBRYO, __FUNCTION__, __LINE__);
  np->state = RUNNABLE;
  if(rc != PRIO_MAX)
    cprintf("Fork isn't moving embryo to RUnable in maxprio");
  if(rc == -1)
    panic("Couldn't find PID in getpriority");
  if(rc < 0 || rc > PRIO_MAX)
    panic("Priority was too large can't add to ready list.");

  stateListAdd(&ptable.ready[rc], np);
  release(&ptable.lock);
  
  return pid;*/

  /*struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  //initialize lists here
  acquire(&ptable.lock);
  initProcessLists();
  initFreeList();
  ptable.PromoteAtTime = ticks + TICKS_TO_PROMOTE;
  release(&ptable.lock);

  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  int rc = getpriority(p->pid);
  acquire(&ptable.lock);
  if (stateListRemove(&ptable.list[EMBRYO], p) == -1) {
    panic("ERROR in userinit!");
  }
  assertState(p, EMBRYO, __FUNCTION__, __LINE__);

  p->state = RUNNABLE;
  if(rc != PRIO_MAX)
    cprintf("Userinit isn't moving embryo to RUnable in maxprio\n");
  if(rc == -1)
    panic("Couldn't find PID in getpriority");
  if(rc < 0 || rc > PRIO_MAX)
    panic("Priority was too large can't add to ready list.");
  stateListAdd(&ptable.ready[rc], p);
  release(&ptable.lock);*/

