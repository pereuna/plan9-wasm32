/* wasm32: 3l's _tas (atomic.rmw.xchg) */
extern int _tas(int*);

int
tas(int *p)
{
	return _tas(p);
}
