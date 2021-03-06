#include "types.h"
#include "defs.h"
#include "memlayout.h"
#include "x86.h"
#include "mmu.h"
#include "proc.h"
#include "vm.h"

extern char data[];

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap_t {
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

pde_t *kpgdir;

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void switchkvm(void) {
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// go through the page directories using virtual address, page allocation is optional
pte_t *walkpgdir(pde_t *pgdir, const void* va, int alloc) {
    // using kernel pagetable
  pde_t *pde = &pgdir[PDX(va)];
  pte_t *pgtbl;
  if(*pde & PTE_P) {
    pgtbl = (pte_t*)P2V(PTE_ADDR(*pde)); // noticed that the address in the pgdir is physical address
  } else {                               // however, our program uses virtual address (mmu helps to translate automatically)
                                         // also, all th kernel memory follows the simple mapping rule P2V.
    if(!alloc || (pgtbl = (pte_t*)kalloc()) == 0) return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtbl, 0, PGSIZE);
    *pde = V2P(pgtbl) | PTE_P | PTE_W | PTE_U; // since we did page alignment in kfree, the address of pgtbl is alredy 4KB aligned (which means the 12 lower bits are 0).
  }
  return &pgtbl[PTX(va)];
}

int mappages(pde_t* pgdir, void* va, uint sz, uint pa, int perm) {
    // using kernel pagetable
  char *p = (char*)PGROUNDDOWN((uint)va);                 // mapping is by pages
  char *end = (char*)PGROUNDDOWN((uint)p+sz-1);           
  while(1) {
    pte_t *pte = walkpgdir(pgdir, p, 1);                  // find the corresponding pagetable or create a new one
    if(pte == 0) return -1;
    if((*pte) & PTE_P) panic("mappages");
    *pte = pa | PTE_P | perm;
    if(p == end) break;
    pa += PGSIZE;
    p += PGSIZE;
  }
  return 0;
}



void kvminit() {
  kpgdir = (pde_t*)kalloc();
  if(!kpgdir) return;
  // make sure all those PDE_P/PTE_P bits are clear
  memset(kpgdir, 0, PGSIZE);
  struct kmap_t *k;
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++) {
    if(mappages(kpgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      panic("kvmalloc");
    }
  }
  switchkvm();
}

pde_t* setupkvm() {
  pde_t *pgdir = kalloc();

  if(!pgdir) return 0;

  memset(pgdir, 0, PGSIZE);
  for(struct kmap_t* k = kmap; k < &kmap[NELEM(kmap)]; k++) {
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      panic("kvmalloc");
    }
  }
  return pgdir;
}

void inituvm(pde_t *pgdir, char *init, uint sz) {
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memcpy(mem, init, sz);
}

void switchuvm(struct proc *p) {
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

pde_t *copyuvm(pde_t *pgdir, uint sz) {
    // using kernel pagetable
    pde_t *newpgdir;

    if((newpgdir = setupkvm()) == 0) {
        return 0;
    }

    for(char *i = 0; i < (char*)sz; i += PGSIZE) {
        pte_t *pte = walkpgdir(pgdir, i, 0);
        if(!pte)
            panic("copyuvm");
        if(!(*pte & PTE_P))
            panic("copyuvm");
        uint pa = PTE_ADDR(*pte);
        uint flags = PTE_FLAGS(*pte);
        char *newpage = kalloc();
        if(!newpage) {
            kfree(newpgdir);
            return 0;
        }
        memcpy(newpage, P2V(pa), PGSIZE);
        if(mappages(newpgdir, i, PGSIZE, V2P(newpage), flags) == -1) {
            kfree(newpage);
            kfree(newpgdir);
            return 0;
        }
    }

    return newpgdir;
}

void freeuvm(pde_t *pgdir) {
    if(!pgdir) panic("freevm");

    deallocuvm(pgdir, KERNBASE, 0);

    for(int i = 0; i < NPDENTRIES; i++){
        if(pgdir[i] & PTE_P){
            char * v = P2V(PTE_ADDR(pgdir[i]));
            kfree(v);
        }
    }

    kfree(pgdir);
}

int allocuvm(pde_t *pgdir, uint oldsz, uint newsz) {
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      panic("allocuvm: out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      panic("allocuvm: out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

int deallocuvm(pde_t *pgdir, uint oldsz, uint newsz) {
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte) // if the pagetable doesn't exist
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE; // we can look to next pagetable (since this pagetable has been empty), -PGSIZE is to cancel the auto increment in for loop
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}