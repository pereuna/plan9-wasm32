#include <u.h>
#include <libc.h>

/*
 * 3a: there is no assembler for wasm32 yet (docs/wasm32.md): what other
 * architectures have in assembler is C here (sys/src/libc/wasm32).
 * This says so, with an error, to whatever expects one: an $AS in a
 * mkfile, a .s file.
 */
void
main(int argc, char **argv)
{
	ARGBEGIN{
	case 'o':
	case 'D':
	case 'I':
		ARGF();
		break;
	}ARGEND
	fprint(2, "3a: no assembler for wasm32 yet; write it in C (docs/wasm32.md)");
	for(; argc > 0; argc--, argv++)
		fprint(2, ": %s", *argv);
	fprint(2, "\n");
	exits("no 3a");
}
