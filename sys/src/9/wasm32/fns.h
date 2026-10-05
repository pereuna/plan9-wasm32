#include <bootinfo.h>
#include "../port/portfns.h"
#include "platform.h"

/*
 * wasm32: what port/ wants of the machine and portfns.h does not declare
 */
void	coherence(void);		/* 3l's: atomic.fence */
int	tas(void*);
int	cmpswap(long*, long, long);
void	idlehands(void);
void	procsave(Proc*);
void	procrestore(Proc*);
void	procsetup(Proc*);
void	procfork(Proc*);
void	evenaddr(uintptr);
void	validalign(uintptr, unsigned);
#define	kmapinval()
#define	kmap(p)		((KMap*)(p)->pa)
#define	kunmap(k)	USED(k)
void	splx(int);
void	cycles(uvlong*);
void	mmuinit(void);
_Noreturn void	clockinit(void);
int	userureg(Ureg*);
char*	getconf(char*);
vlong	syscall(int, ulong);
_Noreturn void	touser(char**, int);
uintptr	sysbind(va_list);
uintptr	sysopen(va_list);
extern Proc	*initp;
int	procspawn(Proc*, int);
void	procunmake(Proc*);
void	procrelease(Proc*);
int	helperspawn(Proc*);
void	helper(void);
long	ainc(long*);
long	adec(long*);
int	helperdie(void);
extern Ufns	ufns;
void	helperquit(Proc*);
void	umemrelease(Proc*);
void	callabort(Proc*);
void	screeninit(void);
void	mouseinput(void);

/* port/bootinfo.c and port/bootargs.c: what the page, the firmware, told us (sys/include/bootinfo.h, docs/boot-abi-wasm32.md) */
extern BootInfo*	bootinfo;
extern uchar*	bootblob;	/* the blob itself, its sections; bootinfo is the header's copy */
extern uintptr	bootinfopa;
void	bootinfoinit(void);
void	bootinforandinit(void);
void	bootinfoclock(void);
BootMem*	bootmem(int);
char*	bootconfig(void);
uchar*	bootfdt(ulong*);
enum { BootInfoArch = BootArchWasm32 };	/* the blob must be made for this ISA */
enum { BootClassRAM, BootClassACPI, BootClassReserved };
int	bootmemclass(u32int);
void	bootargsinit(void);
char*	getconf(char*);
void	setconfenv(void);
/* bootarch.c: wasm32's side of port/bootinfo.c */
void*	bootearlymap(uvlong, uvlong);
void	halt(void);
