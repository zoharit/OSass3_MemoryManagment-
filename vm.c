#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()
char* insert_to_mem(struct proc* p,pde_t *pgdir,uint va);
struct page_data findPageToSwap(struct proc* p) ;
// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}


// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}
// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

// In case we are not the Shell / init proc
  if (myproc() && myproc()->pid > 2) {
  	#ifndef NONE
  	// Enter this function when the swapping policy isn't XV6 default
  	return alloc_pages(pgdir, oldsz, newsz);
  	#endif	
  }
	
  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    if(insert_to_mem(myproc(),pgdir,a) == 0) {
    	deallocuvm(pgdir, newsz, oldsz);
  		return 0;
  	}
    myproc()->count_physical_pages++;
  }
  return newsz;
}


// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t* pte;
  uint a, pa;
  #ifndef NONE
    uint va_memory, va_swapFile;
    struct proc* curproc = myproc();
  #endif

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      #ifndef NONE
        if(curproc->pgdir == pgdir && curproc->pid > 2){
          va_memory = PTE_ADDR(a);
          delete_pyc_page(curproc,va_memory);
        }
      #endif

      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }else if((*pte & PTE_PG) != 0){
      #ifndef NONE
        if(curproc->pgdir == pgdir){
          va_swapFile = PTE_ADDR(a);
          delete_file_page(curproc,va_swapFile);
        }
      #endif
    }
  }
  return newsz;
}
// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}
// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P)&& !(*pte & PTE_PG))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if(*pte & PTE_P){
      if((mem = kalloc()) == 0)
        goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0)
      goto bad;
    }
    else{

       if((pte = walkpgdir(d, (void*)i, 1)) == 0) 
          panic("copyuvm: Can't map swapped page\n");
      

      // The pa isn't relevant here since we will get a page fault
      // anyways , therfore we only need the flags to indicate that
      // the page is in the swap file and the va will already map it
      // to the approptiate location int the swap file. 
      *pte = 0 | flags;
    }
  }
  return d;

bad:
  panic("copyuvm:bad");
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

// PAGEBREAK!
// Blank page.
// PAGEBREAK!
// Blank page.
// PAGEBREAK!
// Blank page.

//***********************************************
//copy physical pages from one process to another
void
copy_pyc_pages(struct proc* proc1,struct proc* proc2)
{
 int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++) {
    proc2->physical_memory_pages[i].va = proc1->physical_memory_pages[i].va;
            if(is_pmalloc((void*)proc1->physical_memory_pages[i].va))
        {
          Update_Pmalloc((void*)proc2->physical_memory_pages[i].va);
        }
  }
  proc2->count_physical_pages = proc1->count_physical_pages;
  proc2->FIFO_INDEX = proc1->FIFO_INDEX;
  proc2->LIFO_INDEX = proc1->LIFO_INDEX;
  proc2->count_total_swap = 0;
  proc2->count_pages_faults = 0;
}

//*****************************************
// copy file pages from one process to another 
void
copy_file_pages(struct proc* proc1,struct proc* proc2)
{
  char* buffer = kalloc();
  int i = 0;
  int j = 0;
  for (; i < MAX_PSYC_PAGES; i++) {
    if(proc1->file_pages[i]!=-1){
      //put the va in proc2
      proc2->file_pages[j] = proc1->file_pages[i];
      //copy the page from proc1 
      int read_byts = readFromSwapFile(proc1,buffer,PGSIZE * i,PGSIZE);
      if(read_byts < PGSIZE) {
        panic("can't read all page\n");
      }
      //write to proc2 the page we coppied from proc1
      int write_bytes = writeToSwapFile(proc2,buffer,PGSIZE * j, PGSIZE);
      if( write_bytes < 0) {
        panic("can't write page to file\n");
      }
      j++;
    }
  }
  proc2->count_file_pages = proc1->count_file_pages;
  kfree(buffer);
  
  //in the rest of the array of proc2 we need to put -1
  for (; j < MAX_FILES_PAGES; j++) {
    proc2->file_pages[j] = -1;
  }
}
//********************************************
//CR2VALUE= register that has the virtual address
// This func check if thr page is in the physical address- if so return 1
int
pageSwapped(struct proc* proc, uint cr2val)
{
  pte_t *pte;
  pte = walkpgdir(proc->pgdir, (void*)cr2val, 0);
  if(!pte || !(*pte & PTE_PG))
    return 0; 
  return 1;
}

//***********************************************

//allcate the process pages
int
alloc_pages(pde_t *pgdir, uint oldsz, uint newsz)
{
  uint a;
	struct proc* p = myproc();
	// if we cant allocate more pages
	if (p->count_file_pages + 
  p->count_physical_pages >= MAX_TOTAL_PAGES)
		panic("no room for more pages for this process\n");

 	a = PGROUNDUP(oldsz);
  	for(; a < newsz; a += PGSIZE){
      //if there is no room on the physical memory
  		if (p->count_physical_pages== MAX_PSYC_PAGES)
         move_Pyc2File(p);

  		if (insert_to_mem(p,pgdir,a) == 0) {
        deallocuvm(pgdir, newsz, oldsz);
  			return 0;
  		}
  		p->count_physical_pages++;
  	}
  	return newsz;
}


char*
insert_to_mem(struct proc* p,pde_t *pgdir,uint va){
    char *mem;
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)va, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      
      kfree(mem);
      return 0;
    }
    #ifndef NONE
    int i=0;
    for(; i < MAX_PSYC_PAGES; i++) {
      //find an empty place
      if(p->physical_memory_pages[i].va == -1){
        p->physical_memory_pages[i].va = (int) PTE_ADDR(va);
    goto found;
      }
    }
    found:
    #endif
    return mem;

}
//********************************************
//if there is no room in both of the arrays- 
//we need to switch between 2
void
make_full_swap(struct proc* proc, uint cr2Val)
{
  char* temp_data_page;

  //just for now choose the firt one
  uint memory_va = findPageToSwap(proc).va;
  //make a room in the physical memory:
  int i=0;
   for(; i < MAX_PSYC_PAGES; i++) {
    if(proc->physical_memory_pages[i].va == memory_va) {
      proc->physical_memory_pages[i].va = -1;
      goto found_memory;
    }
  }
  panic("Page not found in the physical memory\n");
  
  found_memory:
  //now we are going to save in a temp place in the memory 
  //the page that we take out from the physical memory
  temp_data_page = kalloc();
  pte_t* pte = walkpgdir(proc->pgdir, (void*)memory_va, 0);
  memmove((void*)temp_data_page,(void*)PTE_ADDR(pte),PGSIZE);
 
  //change the flag to zero
  *pte = (*pte) & ~PTE_P;

  //move down the counter of the physical memory
  proc->count_physical_pages--;

 //move the page from the file to the physical memory:
 //there is a room there because we just move out one page
 // 15 = array_memory 16 = array_files
  move_File2Pyc(proc,cr2Val);
 // 16= array_memory 15 = array_files

  //after we move the physical memory is full again
  //in the file there is a place- we need to add the temp file to there
  //find the empty place in the file
  for (i = 0; i < MAX_FILES_PAGES; i++) {
    if(proc->file_pages[i] == -1) {
      proc->file_pages[i] = memory_va;
      goto found_file;
    }
  }

 //if there is no room in the file
  panic("can't find space in the file\n");
   
  found_file:
  // writing the page into the file
  writeToSwapFile(proc,(char*) temp_data_page, i*PGSIZE, PGSIZE);
  //change the flag to 1
  *pte = (*pte) | PTE_PG;
  // free the memory that we allocate 
  kfree(temp_data_page);
  proc->count_file_pages++;
  //array files = 16
  lcr3(V2P(proc->pgdir));
}
//********************************************
//move page from file to physical memory
void 
move_File2Pyc(struct proc* proc, uint cr2Val){

  uint va=PTE_ADDR(cr2Val);
  char* memory_alloc;
  //seach for index in the files tbl + delete this page if exist
  int i = 0;
  
  for(;i<MAX_FILES_PAGES ;i++){
    if(proc->file_pages[i] == va){
      proc->file_pages[i]=-1;
      goto found;
    }
  }
  //if the page is not in the file
  panic("Page not found in the file\n");
  
  //else- allocate new place for the page
  found:
  memory_alloc = insert_to_mem(proc,proc->pgdir, va);
  if(memory_alloc == 0)
    panic("allocate fail!\n");
  
  int read_bits=readFromSwapFile(proc, memory_alloc,i*PGSIZE ,PGSIZE);
  if(read_bits !=PGSIZE )
    panic("we didn't read the all page\n");
  
  //we have one more page in the physical memory
  proc->count_physical_pages++;
  //we have one page less in the file
  proc->count_file_pages--;   

  //get the physical address for this page
  pte_t * pyc_add = walkpgdir (proc->pgdir ,(void*) va ,0);

  //change the flag that say that the page is in the files to zero in the physical address.
  *pyc_add = *pyc_add & ~PTE_PG;
  lcr3(V2P (proc->pgdir));
}

void 
move_Pyc2File(struct proc* proc){
  int write_bits;
  //for now we need to take the first one-
  // later we will change it  
  uint va =findPageToSwap(proc).va;
  int i = 0;
  for(;i<MAX_FILES_PAGES ;i++){
    if(proc->file_pages[i] == -1){
      proc->file_pages[i] = va;
      goto found;
    }
  }
  //if there is no room in the file
  panic("can't find space in the file\n");
  
  //else- allocate new place for the page
  found:
  write_bits=writeToSwapFile(proc, (char*)va, i*PGSIZE ,PGSIZE);
  if(write_bits != PGSIZE )
    panic("we didn't write the all page to the file\n");
  
  //we have one less page in the physical memory
  proc->count_physical_pages--;

  //we have one more page in the file
  proc->count_file_pages++;   

  proc->count_total_swap++;

  for(i = 0; i < MAX_PSYC_PAGES; i++) {
    if(proc->physical_memory_pages[i].va == va) {
      proc->physical_memory_pages[i].va = -1;
      break;
    }
  }
    if(i == MAX_PSYC_PAGES) {
    panic("swapOutPage: Can't find the va in physical_memory_pages structure\n");
  }
  //get the physical address for this page
  pte_t * pyc_add = walkpgdir (proc->pgdir ,(void*) va ,0);


  //change the flag that say that the page is in the files to one in the physical address.
  *pyc_add = *pyc_add | PTE_PG;
  //change the flag that say that the page is in the physical memory to zero in the physical address.
  *pyc_add = *pyc_add & ~PTE_P;

  kfree((char*)PTE_ADDR (P2V (*pyc_add)));
  lcr3(V2P (proc->pgdir));
}

//********************************
void
free_file_pages(struct proc* proc)
{
   int i= 0;
    for (; i < MAX_FILES_PAGES; i++) {
      proc->file_pages[i] = -1;
    }
    proc->count_file_pages = 0;

  //remove swap file
  if (removeSwapFile(proc) < 0) {
    panic("can't remove swap file\n");
  }
}

//***********************************************
void
free_pyc_pages(struct proc* proc)
{
  int i= 0;
    for (; i < MAX_PSYC_PAGES; i++) {
      proc->physical_memory_pages[i].va = -1;
    }
    proc->count_physical_pages = 0;
    proc->FIFO_INDEX = 0;
    proc->LIFO_INDEX = 15;
}

//***********************************************

void delete_file_page(struct proc* p, uint va) {
   #ifdef NONE
    return;
  #endif
  int i=0;
  pte_t* pte;
  for (; i < MAX_FILES_PAGES; i++) {
    if (p->file_pages[i] == va) {
      p->file_pages[i] = -1;
      goto found;
    }
  }
  panic("can't find the page\n");
  found:
   pte = walkpgdir(p->pgdir, (void*)va ,0);
  if(pte) {
    *pte = *pte & ~PTE_PG;
  }
  p->count_file_pages--;
}
//*****************************************
void
delete_pyc_page(struct proc* curproc, uint va){
   #ifdef NONE
    return;
  #endif
  pte_t* pte_memory;
  int i;
  if((pte_memory = walkpgdir(curproc->pgdir, (void *) va, 0)) != 0)
    *pte_memory &= ~PTE_P;  // clear present flag
  for(i = 0; i < MAX_PSYC_PAGES; i++)
    if(curproc->physical_memory_pages[i].va == va)
      curproc->physical_memory_pages[i].va = -1;
  curproc->count_physical_pages--;
}

//*****************************************



int checkUserPage(pde_t* pgdir, void* va) {
  pte_t* pte = walkpgdir(pgdir, va, 0);
    if((*pte & PTE_U) == 0) {
      return 0;
    }
    return 1;
}

//*****************************************

int isAccessFlagOn(struct proc* p, uint va) {
  pte_t* pte;
  int pte_a;
  pte = walkpgdir(p->pgdir,(void*)va, 0);
  pte_a = PTE_FLAGS(*pte) & PTE_A;
  *pte = *pte & ~PTE_A;
  return pte_a;
}
//*****************************************

struct page_data SCFIFO_ALG(struct proc* p) {
    int toSwapIndex = -1;
    //a number between 0-15
    int i = (p->FIFO_INDEX) % MAX_PSYC_PAGES;
    
    while (1) {

      if (checkUserPage(p->pgdir,(void*) p->physical_memory_pages[i].va)) {

        if (isAccessFlagOn(p, p->physical_memory_pages[i].va) == 0) {
          toSwapIndex = i;
            cprintf("indexToSwap: %d\n", toSwapIndex);

          break;
        }
      }
      //moving to the next page becuse the bit of this one is on
      i = (i + 1) % MAX_PSYC_PAGES;  
    }
    //found the page we want to get out
    //the next time we will strat with the next page
    p->FIFO_INDEX = (i + 1) % MAX_PSYC_PAGES;
    return p->physical_memory_pages[toSwapIndex];
}

//*****************************************

struct page_data
LIFO_ALG(struct proc* p)
{ 
  int indexToSwap = -1;
  if(p->LIFO_INDEX < 0)
    indexToSwap = ((p->LIFO_INDEX) % MAX_PSYC_PAGES)+16;
  else
    indexToSwap = (p->LIFO_INDEX) % MAX_PSYC_PAGES;

  cprintf("indexToSwap: %d\n", indexToSwap);
  indexToSwap--;
  p->LIFO_INDEX = indexToSwap % MAX_PSYC_PAGES;
  indexToSwap++;
  return p->physical_memory_pages[indexToSwap];
}
//*****************************************

struct page_data
findPageToSwap(struct proc* p) {
  cprintf("swaping \n");
  #ifdef SCFIFO
  return SCFIFO_ALG(p);
  #elif LIFO
  return LIFO_ALG(p);
  #endif
  return p->physical_memory_pages[0];
}

void
Update_Pmalloc(void * va){
  pte_t* pte;
  pte = walkpgdir(myproc()->pgdir,(void*)va, 0);
  *pte = *pte | PTE_PMAL;
}

int
is_pmalloc(void* va){
  pte_t* pte;
  pte = walkpgdir(myproc()->pgdir,(void*)va, 0);
  if(*pte & PTE_PMAL){
    return 1;
  }

 return 0;
}


int protected_page(void* va) 
{
  pte_t* pte = walkpgdir(myproc()->pgdir,va,0);
  if((*pte & PTE_PMAL) && !((uint)va & 0xFFF))
  {
    *pte = *pte & ~PTE_W;
    myproc()->count_protected_pages++;
    return 1;
  }
  return -1;
}

int
is_protected(void* va){
  pte_t* pte;
  pte = walkpgdir(myproc()->pgdir,(void*)va, 0);
  if(*pte & PTE_W){
    return 0;
  }

 return 1;
}

void
update_unprotected(void* va){
  pte_t* pte;
  pte = walkpgdir(myproc()->pgdir,(void*)va, 0);
  *pte = *pte | PTE_W;
}

void 
pmalloc_Off(void *va){
    myproc()->count_protected_pages--;
    pte_t* pte;
  pte = walkpgdir(myproc()->pgdir,(void*)va, 0);
  *pte = *pte & ~PTE_PMAL;
}