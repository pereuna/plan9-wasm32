#include <u.h>
#include <libc.h>

/*
 * 3c test: the system call ABI (_trap(n, &first argument)): every
 * argument where the kernel looks for it, the vlongs in pread, pwrite and
 * seek too; sysabi.out was written by hand from what the calls mean
 */
void
main(int, char**)
{
	int fd, n;
	char buf[64];
	vlong o;
	char *f;

	f = "/tmp/3c-sysabi.tmp";
	fd = create(f, ORDWR, 0644);
	print("create %s\n", fd >= 0 ? "ok" : "failed");
	n = write(fd, "0123456789abcdef", 16);
	print("write %d\n", n);
	o = seek(fd, 4LL, 0);
	print("seek set %lld\n", o);
	n = read(fd, buf, 3);
	buf[n] = 0;
	print("read %d %s\n", n, buf);
	o = seek(fd, 2LL, 1);
	print("seek cur %lld\n", o);
	n = read(fd, buf, 2);
	buf[n] = 0;
	print("read %d %s\n", n, buf);
	n = pread(fd, buf, 4, 10LL);
	buf[n] = 0;
	print("pread %d %s\n", n, buf);
	n = pwrite(fd, "XY", 2, 1LL);
	print("pwrite %d\n", n);
	o = seek(fd, -2LL, 2);
	print("seek end %lld\n", o);
	n = read(fd, buf, 10);
	buf[n] = 0;
	print("read %d %s\n", n, buf);
	n = pread(fd, buf, sizeof buf - 1, 0LL);
	buf[n] = 0;
	print("pread all %d %s\n", n, buf);
	o = seek(fd, 1LL<<33, 0);
	print("seek far %lld\n", o);
	print("close %d\n", close(fd));
	print("close again %d\n", close(fd));
	fd = open("/tmp/3c-sysabi.none/x", OREAD);
	print("open missing %s\n", fd < 0 ? "failed" : "ok");
	print("remove %d\n", remove(f));
	print("remove again %s\n", remove(f) < 0 ? "failed" : "ok");
	exits(nil);
}
