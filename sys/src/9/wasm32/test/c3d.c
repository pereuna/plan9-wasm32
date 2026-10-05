#include <u.h>
#include <libc.h>

/*
 * C3d (tools/test-9wasm32): the page's mouse and cursor - a cursor of
 * its own into /dev/cursor (the page's canvas shows it), then a click on
 * the screen read from /dev/mouse: its button and which half it was in.
 */
void
main(void)
{
	char buf[64], *f[6];
	uchar curs[2*4+2*2*16];
	int fd, cfd, down, b, x;

	if((cfd = open("/dev/cursor", OWRITE)) < 0)
		sysfatal("/dev/cursor: %r");
	memset(curs, 0, sizeof curs);
	curs[3] = -1;	/* offset (-1, -1), big-endian */
	memset(curs, 0xff, 4);
	memset(curs+4, 0xff, 4);
	memset(curs+8+32, 0xaa, 32);	/* set: stripes */
	if(write(cfd, curs, sizeof curs) != sizeof curs)
		sysfatal("write /dev/cursor: %r");
	if((fd = open("/dev/mouse", OREAD)) < 0)
		sysfatal("/dev/mouse: %r");
	print("ready\n");
	down = 0;
	for(;;){
		if(read(fd, buf, 49) != 49)
			sysfatal("read /dev/mouse: %r");
		buf[49] = 0;
		if(buf[0] != 'm' || tokenize(buf+1, f, nelem(f)) < 4)
			continue;
		x = atoi(f[0]);
		b = atoi(f[2]);
		if(b){
			if(!down)
				print("button %d in the %s half\n", b, x < 400 ? "left" : "right");
			down = 1;
		}else if(down)
			break;
	}
	print("up\n");
	close(cfd);
	exits(nil);
}
