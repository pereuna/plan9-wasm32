#include "../plan9/lib.h"
#include "../plan9/sys9.h"
#include <stdlib.h>

typedef long long vlong;
typedef unsigned long ulong;
typedef unsigned long long uvlong;
#include "/sys/include/tos.h"

/*
 * wasm32: the kernel puts argc, argv[0] ... nil at SP and calls _main
 * (3l's _start): they are _main's parameters, &arg0 is argv.  What
 * plan9/callmain.c's _callmain does, here: it would get a copy of arg0,
 * not the kernel's
 */
extern	int	_apemain(int, char**);
extern	void	**_privates;
extern	int	_nprivates;
extern	char	*_plan9err;
extern	int	*_errnoloc;

Tos	*_tos;

enum {
	NPRIVATES = 16,
};

/*
 * what is each proc's own (libc's wasm32/main9.c): the kernel swaps it
 * as rfork(RFMEM) procs take turns, and puts the pid in tos
 */
struct {
	Tos	tos;
	void	*priv[NPRIVATES];
} _perproc;

void
_main(int argc, char *arg0)
{
	int errno;
	char err[ERRMAX];

	err[0] = '\0';
	_tos = &_perproc.tos;
	_privates = _perproc.priv;
	_nprivates = NPRIVATES;
	_errnoloc = &errno;
	_plan9err = err;
	exit(_apemain(argc, &arg0));
}
