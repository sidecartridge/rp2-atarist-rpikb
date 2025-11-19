/* <<<                                  */
/* Copyright (c) 1994-1996 Arne Riiber. */
/* All rights reserved.                 */
/* >>>                                  */

/*
 *  Memory access
 */
#ifdef unix
#include <malloc.h>
#endif

#ifdef __MSDOS__
#include <alloc.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <stdio.h>

#include "chip.h" /* chip specific: NIREGS */
#include "defs.h" /* general definitions */
#include "ireg.h" /* ireg_getb/putb_func[], ireg_start/end */
#include "memory.h"

/*
 * Memory variables
 *
 * Addresses used by mem_getb/putb should be set with command line options.
 * Disabling internal regs can be done by declaring intreg_start > intreg_end
 */
u_int ram_start; /* 0x0000; */
u_int ram_end;   /* 0xFFFF; */
u_char *ram = 0; /* was [65536]; modified for MSDOS compilers */
/*
 * mem_init - initialize memory area
 */
u_char *mem_init() {
  u_int size = (256 + 4096);
  if (ram == NULL) {
    // allocate extra to allow alignment + store original pointer
    void *original = malloc(size + 4 + sizeof(void *));
    if (original == NULL) {
      perror("Couldn't allocate ram");
      return NULL;
    }
    uintptr_t ptr = (uintptr_t)original + sizeof(void *);
    uintptr_t aligned = (ptr + (4 - 1)) & ~(uintptr_t)(4 - 1);
    void **store = (void **)(aligned - sizeof(void *));
    *store = original;
    ram = (u_char *)aligned;

    DPRINTF("6301: ram %d allocated OK\n", size);
    ram_start = 0;
    ram_end = MEMSIZE - 1;
    memset(ram, 0, 256);
  } else {
    printf("ram already allocated\n");
  }
  return ram;
}

int mem_free() {
  if (ram) {
    void **store = (void **)((uintptr_t)ram - sizeof(void *));
    void *original = *store;
    free(original);
    ram = NULL;
  }
  return 0;
}

/*
 * mem_inramrom - returns 1 if valid ram or rom address
 *
 * Used by mem print routines
 */

static mem_inramrom(addr) u_int addr;
{
  return 1;
}

/*
 * mem_print_ascii - print ram/rom memory area in ascii
 */
mem_print_ascii(startaddr, nbytes) u_int startaddr;
u_int nbytes;
{
  u_int i, addr, value;

  for (i = 0, addr = startaddr; i < nbytes; i++, addr++)
    if (mem_inramrom(addr))
      if (isprint(value = ((addr < NIREGS) ? iram[addr] : ram[addr])))
        putchar(value);
      else
        putchar('.');
}

/*
 * mem_print - CPU independent memory dump (print only ram/rom area)
 */
mem_print(startaddr, nbytes, nperline) u_int startaddr;
u_int nbytes;
u_int nperline;
{
  u_int i, j, addr;

  for (i = 0, addr = startaddr; i < nbytes;) {
    printf("%04x\t", addr);
    for (j = 0; j < nperline; i++, j++, addr++)
      if (i >= nbytes)
        printf("   ");
      else if (mem_inramrom(addr)) {
        if (addr < NIREGS)
          printf("%02x ", iram[addr]);
        else
          printf("%02x ", ram[addr]);
      } else
        printf("-- ");
    putchar(' ');
    mem_print_ascii(addr - nperline, (nbytes < nperline) ? nbytes : nperline);
    putchar('\n');
  }
  return addr;
}
