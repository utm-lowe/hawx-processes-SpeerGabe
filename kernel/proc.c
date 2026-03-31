#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "mem.h"
#include "string.h"
#include "console.h"
#include "trap.h"
#include "proc.h"

// Extern Globals
extern pagetable_t kernel_pagetable; // mem.c
extern char trampoline[]; // trampoline.S
extern char _binary_user_init_start; // The user init code

////////////////////////////////////////////////////////////////////////////////
// Static Definitions and Helper Function Prototypes
////////////////////////////////////////////////////////////////////////////////
static int nextpid = 1;
static pagetable_t proc_pagetable(struct proc*);
static void proc_free_pagetable(pagetable_t pagetable, uint64 sz);
static void proc_freewalk(pagetable_t pagetable);
static uint64 proc_shrink(pagetable_t pagetable, uint64 oldsz, uint64 newsz);
static int proc_loadseg(pagetable_t pagetable, uint64 va, void *bin, uint offset, uint sz);
static void proc_guard(pagetable_t pagetable, uint64 va);


////////////////////////////////////////////////////////////////////////////////
// Global Definitions
////////////////////////////////////////////////////////////////////////////////
struct cpu cpu;
struct proc proc[NPROC];




////////////////////////////////////////////////////////////////////////////////
// Process API Functions 
////////////////////////////////////////////////////////////////////////////////

// Initialize the proc table, and allocate a page for each process's 
// kernel stack. Map the stacks in high memory, followed by an invalid guard page.
void 
proc_init(void)
{
    // You need to loop over all the proc structs and set up their stacks
    // This setup requires two steps:
    //   1.) Use the KSTACK macro to set up the kstack field in the struct
    //   2.) Allocate a new physical page for the stack and insert it
    //       into the kernel's page table at the virtual address referred
    //       to by kstack.
    // HINTS: This function is a combination of two functions in xv6. 
    //        I used the following memory functions:
    //           vm_page_alloc
    //           vm_page_insert
    // YOUR CODE HERE
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &proc[i];
        p->state = UNUSED;

        // Allocate the kernel stack page
        p->kstack = (uint64)vm_page_alloc();
        if (!p->kstack)
            panic("out of memory for kstack");

        // Compute virtual address for the top of this process's kernel stack
        uint64 kstack_va = TRAMPOLINE - (i + 1) * 2 * PGSIZE;

        // Map the stack into the kernel page table
        if (vm_page_insert(kernel_pagetable, kstack_va, p->kstack, PTE_W | PTE_R) < 0)
            panic("vm_page_insert failed for kstack");
}
}



// Set up the first user process. Return the process it was allocated to.
struct proc*
proc_load_user_init(void)
{
    void *bin = &_binary_user_init_start;

    // Allocate a new process. If there is no process avaialble, panic.
    // Use proc_load_elf to load up the elf string. 
    // As an additional hint, I have defined the variables you need 
    // for you. The bin pointer points to the embedded BLOB which
    // contains the program image for init.
    // YOUR CODE HERE
    struct proc *p = proc_alloc();
    if(p == 0)
        panic("proc_load_user_init: no free process");

    if(proc_load_elf(p, bin) < 0)
        panic("proc_load_user_init: failed to load init ELF");

    // Trapframe already set in proc_load_elf
    p->state = RUNNABLE;
    return p;
}


// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc* 
proc_alloc(void)
{
    // Search for an unused process in the proc array. If you do 
    // not find one, return 0. If you do find one, do the following:
    //   1.) Set the pid field to the next available pid. 
    //       (be sure to update nextpid)
    //   2.) Allocate a trapframe page for the proces.
    //   3.) Set the trapframe page to all zeroes.
    //   4.) Allocate an empty page table for the process.
    //   5.) Set the return address in the processe's context to 
    //       return to usertrapret.
    //   6.) Set the process stack pointer to one address past the end of the
    //       kstack page.
    // HINTS: This function combines several ideas from xv6 function, but it 
    //        does require adaptation. 
    //        I used the following functions:
    //          vm_pagealloc
    //          proc_free
    //          memset
    //          proc_pagetable
    // YOUR CODE HERE
    struct proc *p;
    for(int i = 0; i < NPROC; i++){
        p = &proc[i];
        if(p->state == UNUSED){
            p->state = USED;
            p->pid = nextpid++;
            
            //Trapframe allocation
            p->trapframe = (struct trapframe*)vm_page_alloc();
            if(!p->trapframe){
                p->state = UNUSED;
                return 0;
            }
            memset(p->trapframe, 0, PGSIZE);

            //pagetable allocation
            p->pagetable = proc_pagetable(p);
            if(!p->pagetable){
                vm_page_free(p->trapframe);
                p->state = UNUSED;
                return 0;
            }

            p->context.ra = (uint64)usertrapret;  // trampoline return
            p->context.sp = p->kstack + PGSIZE;   // top of stack
            return p;
        }
    }
    return 0;
}


// free a proc structure and the data hanging from it,
// including user pages.
void 
proc_free(struct proc *p)
{
    // Free the process's trapframe, empty its pagetable,
    // and reset all fields to zero. The state of the process
    // should be "UNUSED".
    // HINT: Functions I used
    //         vm_page_free
    //         proc_free_pagetable
    // YOUR CODE HERE
    if (!p) return;

    // Free trapframe
    if (p->trapframe)
        vm_page_free((void*)p->trapframe);

    // Free kernel stack
    if (p->kstack)
        vm_page_free((void*)p->kstack);

    // Free user page table
    if (p->pagetable)
        proc_free_pagetable(p->pagetable, p->sz);

    // Clear the process struct
    memset(p, 0, sizeof(*p));

    // Mark UNUSED at the very end
    p->state = UNUSED;
}


// Load the ELF program image stored in the binary string bin
// into the specified process. This operation will destroy the 
// pagetable currently in p, and replace it with a page table
// as indicated by the segments of the elf formatted binary.
int
proc_load_elf(struct proc *p, void *bin)
{
    struct elfhdr elf;
    struct proghdr ph;
    int i, off;
    uint64 sz=0, sp=0;
    pagetable_t pagetable=0;

    // get the elf header from bin
    elf = *(struct elfhdr*) bin;

    // check the elf magic
    if(elf.magic != ELF_MAGIC)
        goto bad;

    // We need to load the process from the binary string pointed to
    // by bin. This is similar to xv6's exec function, but with 
    // several key differences:
    //   - We are loading from bin, not from the disk.
    //   - Offsets give the number of bytes from the beginning of bin.
    //   - We will not be putting program arguments on the stack.
    // The basic steps we need to perform are as follows:
    //   1.) Create a new pagetable for the process
    //   2.) Loop over all of the program headers in the elf object
    //       - Check the validity of the header, goto bad if invalid
    //       - Use proc_resize to increase the size of the process to 
    //         hold this segment's data
    //       - Use proc_loadseg to load this segment into user memory
    //   3.) Set up the user stack
    //   4.) Destroy the old page table
    //   5.) Commit to the user image
    //   6.) Mark the process as runnable
    //   7.) Return 0
    //
    // If we have bad elf image, do the following:
    //   1.) Free the page table if it exists
    //   2.) return -1
    // HINT: The key to this function is to fully understand how the 
    //       elf object looks in the binary object. I have given you
    //       my variables as well as how you obtain the elf header
    //       as a hint. You will also need to fully understand how
    //       exec works in xv6. Happy reading!
    // YOUR CODE HERE

    // 1) Create new pagetable
    pagetable = proc_pagetable(p);
    if (pagetable == 0)
        goto bad;

    // 2) Load each program segment
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        ph = *(struct proghdr*)((char*)bin + off);

        if (ph.type != ELF_PROG_LOAD)
            continue;

        // Sanity checks
        if (ph.memsz < ph.filesz) goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr) goto bad;

        // Grow process memory to hold segment
        if (proc_resize(pagetable, sz, ph.vaddr + ph.memsz) == 0)
            goto bad;

        // Load segment into memory
        if (proc_loadseg(pagetable, ph.vaddr, bin, ph.off, ph.filesz) < 0)
            goto bad;

        // Update process size
        if (sz < ph.vaddr + ph.memsz)
            sz = ph.vaddr + ph.memsz;
    }

    // 3) Set up user stack: 2 pages (1 guard, 1 stack)
    uint64 stack_base = PGROUNDUP(sz);
    if(proc_resize(pagetable, sz, stack_base + 2*PGSIZE) == 0)
        goto bad;

    // Guard page at first page
    proc_guard(pagetable, stack_base);

    // Stack pointer at top of second page
    sp = stack_base + 2*PGSIZE;
    sp &= ~0xF;

    // 4) Commit new image
    proc_free_pagetable(p->pagetable, p->sz);
    p->pagetable = pagetable;
    p->sz = sz + 2*PGSIZE;

    // 5) Initialize trapframe
    p->trapframe->epc = elf.entry;
    p->trapframe->sp  = sp;

    // 6) Mark runnable
    p->state = RUNNABLE;

    return 0;

  bad:
      if (pagetable)
          proc_free_pagetable(pagetable, sz);
      return -1;
}


// Resize the process so that it occupies newsz bytes of memory.
// If newsz > oldsz
//   Allocate PTEs and physical memory to grow process from oldsz to
// If newsz < oldsz
//   Use proc_shrink to decrease the zie of the process.
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 proc_resize(pagetable_t pagetable, uint64 oldsz, uint64 newsz) 
{
    // Make this behave as above. This is a little bit different from the
    // xv6 equivalent. What did I change? 
    //
    // YOUR CODE HERE
    if(newsz > oldsz){
        for(uint64 a = PGROUNDUP(oldsz); a < PGROUNDUP(newsz); a += PGSIZE){
            void *mem = vm_page_alloc();
            if(!mem) return 0;
            memset(mem, 0, PGSIZE);
            if(vm_page_insert(pagetable, a, (uint64)mem, PTE_W | PTE_R | PTE_U) < 0){
                vm_page_free(mem);
                return 0;
            }
        }
    } else if(newsz < oldsz){
        return proc_shrink(pagetable, oldsz, newsz);
    }
    return newsz;
}


// Given a parent process's page table, copy its memory into a 
// child's page table. Copies both the page table and the physical
// memory.
int 
proc_vmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  // This function is pretty close to the xv6 version, though we use 
  // the functions we wrote in mem.c. (See the mem.h file for the
  // documetnation on these functions.) What this function does is
  // the following:
  // 1.) It goes through every page in the old processes page table.
  // 2.) It allocates new memory for the new process page table.
  // 3.) It copies the memory from the old process to the new process.
  // 4.) It maps this new memory to the new process.
  // You should also make sure to handle errors as was done in the xv6
  // table.
  // YOUR CODE HERE
  for (uint64 addr = 0; addr < sz; addr += PGSIZE) {
        // Get the PTE for this virtual address in the old page table
        pte_t *pte_old = walk_pgtable(old, addr, 0);
        if (!pte_old || !(*pte_old & PTE_V))
            continue; // skip unmapped pages

        // Get physical address of old page
        uint64 pa_old = PTE2PA(*pte_old);

        // Allocate a new page for the child
        void *mem = vm_page_alloc();
        if (!mem)
            return -1;

        // Copy the contents from the parent page
        memmove(mem, (void*)pa_old, PGSIZE);

        // Get permissions from old PTE
        int perm = PTE_FLAGS(*pte_old) & (PTE_U | PTE_R | PTE_W | PTE_X);

        // Map new page into the child's page table
        if (vm_page_insert(new, addr, (uint64)mem, perm) < 0) {
            vm_page_free(mem);
            return -1;
        }
    }
    return 0;
}


////////////////////////////////////////////////////////////////////////////////
// Static Helper Functions
////////////////////////////////////////////////////////////////////////////////

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
static pagetable_t 
proc_pagetable(struct proc *p)
{
    // Create a new pagetable for the process. Do not assign it yet, just
    // return the pagetable after you create it.
    // The page table should contain the following entries:
    //   - Map the trampoline physical address to the TRAMPOLINE virtual address.
    //     trampoline should be readable and executable.
    //   - Map the p->trapframe physical address to the TRAPFRAME virtual address.
    //     The trapframe page should be readable and writable.
    // The functions I used here were:
    //    vm_create_pagetable
    //    vm_page_insert
    //    vm_page_free
    //    vm_page_remove
    // YOUR CODE HERE
    pagetable_t pt = vm_create_pagetable();
    if(pt == 0)
        return 0;

    // Map trampoline
    if (vm_page_insert(pt, TRAMPOLINE, (uint64)trampoline, PTE_X | PTE_R) < 0){
        vm_page_free(pt);
        return 0;
    }

    // Map trapframe
    if(vm_page_insert(pt, TRAPFRAME, (uint64)p->trapframe, PTE_W | PTE_R) < 0){
        vm_page_free(pt);
        return 0;
    }

    return pt;
}



// Free a process's page table, and free the
// physical memory it refers to.
static void 
proc_free_pagetable(pagetable_t pagetable, uint64 sz)
{
    // 1.) Remove the TRAMPOLINE and TRAPFRAME pages
    // 2.) Remove all the user memory pages, freeing their 
    //    physical memory.
    // 3.) Free the user page table.
    // Functions Used: vm_page_remove, proc_freewalk
    // YOUR CODE HERE
    if (!pagetable)
        return;

    // 1) Remove TRAMPOLINE and TRAPFRAME mappings
    vm_page_remove(pagetable, TRAMPOLINE, 1, 0); // trampoline: do not free physical page
    vm_page_remove(pagetable, TRAPFRAME, 1, 0);  // trapframe: do not free physical page

    // 2) Remove all user memory pages
    if (sz > 0){
        uint64 npages = (PGROUNDUP(sz)) / PGSIZE;
        vm_page_remove(pagetable, 0, npages, 1); // free user pages
    }

    // 3) Free the top-level page table itself (recursively)
    proc_freewalk(pagetable);
}



// Recursively free page-table pages.
// All leaf mappings must already have been removed.
static void 
proc_freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      proc_freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  vm_page_free((void*)pagetable);
}


// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
static uint64 
proc_shrink(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    vm_page_remove(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}


// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
proc_loadseg(pagetable_t pagetable, uint64 va, void *bin, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  // Load the program segment from the bin array. Note that 
  // offset could be thought of as the offset into bin. There 
  // is an equivalent xv6 function which does this using inode 
  // loading. The secret to converting it is you are going to 
  // have a line that uses memmove and the following expression:
  //   bin+offset+i
  // As an added hint, I have included my variable declarations 
  // above.
  // YOUR CODE HERE
  char *src = (char*)bin + offset; // pointer to ELF segment data

    // Loop over the segment in page-sized increments
    for (i = 0; i < sz; i += PGSIZE) {
        pte_t *pte = walk_pgtable(pagetable, va + i, 0);
        if (!pte || !(*pte & PTE_V))
            return -1;

        pa = PTE2PA(*pte);

        n = PGSIZE;
        if (sz - i < PGSIZE)
            n = sz - i;

        // Copy into physical page, handle offset within page
        memmove((void*)(pa + ((va + i) % PGSIZE)), src + i, n);
    }

    return 0;
}


// mark a PTE invalid for user access.
// used by proc_load_elf for the user stack guard page.
static void 
proc_guard(pagetable_t pagetable, uint64 va)
{
    pte_t *pte;

    pte = walk_pgtable(pagetable, va, 0);
    if(pte == 0)
        panic("proc_guard");
    *pte &= ~PTE_U;
}

// Find the process with the given pid and return a pointer to it.
// If the process is not found, return 0
struct proc *proc_find(int pid) {
  // Simply search the proc array, looking for the specified pid.
  // YOUR CODE HERE
  for(int i=0; i<NPROC; i++){
    if(proc[i].pid == pid){
      return &proc[i];
    }
  } 
  return 0;
}