#include <u.h>
#include <libc.h>

/*
 * (tools/test-9wasm32) a fault - an indirect call through a pointer
 * that is no function, a trap in WebAssembly: the program ends with
 * sys: trap, the machine goes on
 */
void
main(int argc, char **argv)
{
	void (*f)(void);

	if(argc > 1){
		f = (void(*)(void))(uintptr)100000;
		f();
	}
	print("no fault\n");
	exits(nil);
}
