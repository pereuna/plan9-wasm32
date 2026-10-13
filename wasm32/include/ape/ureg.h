#ifndef __UREG_H
#define __UREG_H
#if !defined(_PLAN9_SOURCE)
    This header file is an extension to ANSI/POSIX
#endif

/*
 * wasm32: no registers to save; notes see this (/wasm32/include/ureg.h)
 */
struct Ureg
{
	unsigned long	pc;
	unsigned long	sp;
};

#endif
