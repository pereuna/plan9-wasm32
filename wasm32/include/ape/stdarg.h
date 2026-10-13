#ifndef __STDARG
#define __STDARG

/* wasm32 (3c): arguments in 4-byte slots, vlong and double in two (u.h's) */
typedef char *va_list;

#define va_start(list, start) list = (sizeof(start)<4 ? (char *)((int *)&(start)+1) : \
(char *)(&(start)+1))
#define va_end(list)
#define va_arg(list, mode) \
	((sizeof(mode) == 1)? \
		((list += 4), (mode*)list)[-4]: \
	(sizeof(mode) == 2)? \
		((list += 4), (mode*)list)[-2]: \
		((list += sizeof(mode)), (mode*)list)[-1])
#define va_copy(dst, src) ((dst) = (src))
#endif /* __STDARG */
