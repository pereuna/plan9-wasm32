#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

/*
 * wasm32's side of port/bootinfo.c (Plan2001 Boot ABI v1,
 * docs/boot-abi-wasm32.md): the page is the firmware and the loader -
 * it makes the BootInfo blob in the machine's memory and enters the
 * kernel at _start with the blob's address as main's argument.  The
 * kernel's addresses are the memory's own (no MMU), so the early map is
 * the address itself, if the memory has it: what lies past its end
 * would trap, so it is refused here instead.
 */
uintptr bootinfopa;

void*
bootearlymap(uvlong pa, uvlong size)
{
	if(pa == 0 || pa + size < pa || pa + size > platmemsize())
		return nil;
	return (void*)(uintptr)pa;
}

/* a blob the kernel cannot take (port/bootinfo.c): the machine stops, the page says so */
void
halt(void)
{
	static long never;
	static int said;

	if(!said++)
		plathalt("bootinfo: no blob this kernel can take");
	platwait(&never, 0, -1);
}
