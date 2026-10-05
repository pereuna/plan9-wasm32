#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

/*
 * wasm32: no mount cache (port/cache.c keeps file data in pages)
 */
void	cinit(void) { }
int	copen(Chan*) { return 0; }
void	cclunk(Chan*) { }
int	cread(Chan*, uchar*, int, vlong) { return 0; }
void	cupdate(Chan*, uchar*, int, vlong) { }
void	cwrite(Chan*, uchar*, int, vlong) { }
void	cachedel(Image*, uintptr) { }
void	ctrunc(Chan*) { }
