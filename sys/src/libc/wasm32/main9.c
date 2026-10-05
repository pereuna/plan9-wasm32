#include <u.h>
#include <libc.h>
#include <tos.h>

/*
 * wasm32: the kernel puts argc, argv[0] ... nil at SP and calls _main
 * (3l's _start): they are _main's parameters, &arg0 is argv
 */
char	*argv0;
Tos	*_tos;
void	**_privates;
int	_nprivates;

enum {
	NPRIVATES = 16,
};

/*
 * what is each proc's own on Plan 9 (its stack, at the same address in
 * each): _tos and privalloc's.  Here rfork(RFMEM) procs share memory, so
 * the kernel keeps this per proc, swapping it when they take turns (3l
 * exports its address); it puts the pid in tos.
 */
struct {
	Tos	tos;
	void	*priv[NPRIVATES];
} _perproc;

extern	void	main(int, char**);	/* the program's: libc.h does not declare it, and -T wants it */

void
_main(int argc, char *arg0)
{
	_tos = &_perproc.tos;
	_privates = _perproc.priv;
	_nprivates = NPRIVATES;
	main(argc, &arg0);
	exits("main");
}
