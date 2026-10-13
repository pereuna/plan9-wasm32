#ifndef __SETJMP_H
#define __SETJMP_H
#pragma lib "/$M/lib/ape/libap.a"

typedef int jmp_buf[10];
#ifdef _POSIX_SOURCE
typedef int sigjmp_buf[10];
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern int setjmp(jmp_buf);
extern void longjmp(jmp_buf, int);

#ifdef _POSIX_SOURCE
extern int sigsetjmp(sigjmp_buf, int);
extern void siglongjmp(sigjmp_buf, int);

/*
 * wasm32: 3l makes setjmp in place at each call, in its caller's frame
 * (WebAssembly exceptions), so sigsetjmp cannot be a function that
 * calls it: the mask and its saved value, then setjmp on the rest
 * (siglongjmp: longjmp(buf+2))
 */
extern long _psigblocked;
#define sigsetjmp(buf, savemask) ((buf)[0] = (savemask), (buf)[1] = _psigblocked, setjmp(&(buf)[2]))
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SETJMP_H */
