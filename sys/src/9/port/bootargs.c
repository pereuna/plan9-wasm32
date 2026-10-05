#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

#define	MAXCONF 64
static char *confname[MAXCONF];
static char *confval[MAXCONF];
static int nconf;

static char acpibuf[24];
static char fbbuf[128];
static char ncpubuf[16];
static char chosenbuf[1024];

/* set name to val, replacing an earlier value; the strings are not copied */
static void
addconf(char *name, char *val)
{
	int j;

	for(j = 0; j < nconf; j++){
		if(cistrcmp(confname[j], name) == 0)
			break;
	}
	if(j == MAXCONF)
		return;
	confname[j] = name;
	confval[j] = val;
	if(j == nconf)
		nconf++;
}

/*
 * The firmware's device tree (BootInfo's FDT section, if there is one:
 * ARM64 and RISC-V firmware, not PCs), for what the kernel's config needs
 * of it: the number of processors (*ncpu, unless plan9.ini sets it) and
 * /chosen's bootargs (name=value words, after plan9.ini's).  From 9front's
 * arm64 bootargs.c.
 */
enum {
	DtHeader	= 0xd00dfeed,
	DtBeginNode	= 1,
	DtEndNode	= 2,
	DtProp		= 3,
	DtNop		= 4,
};

typedef struct Devtree Devtree;
struct Devtree
{
	uchar	*end;
	char	*stab;
	char	path[1024];
	int	ncpu;
};

static u32int
beget4(uchar *p)
{
	return (u32int)p[0]<<24 | (u32int)p[1]<<16 | (u32int)p[2]<<8 | (u32int)p[3];
}

static void
devtreeprop(Devtree *t, char *key, uchar *val, int len)
{
	char *toks[MAXCONF], *v;
	int i, n;

	if(strncmp(t->path, "/cpus/cpu", 9) == 0 && strcmp(key, "reg") == 0){
		t->ncpu++;
		return;
	}
	if(strcmp(t->path, "/chosen") == 0 && strcmp(key, "bootargs") == 0){
		if(len >= sizeof chosenbuf)
			len = sizeof chosenbuf - 1;
		memmove(chosenbuf, val, len);
		chosenbuf[len] = 0;
		n = tokenize(chosenbuf, toks, MAXCONF);
		for(i = 0; i < n; i++){
			if((v = strchr(toks[i], '=')) == nil)
				continue;
			*v++ = 0;
			addconf(toks[i], v);
		}
	}
}

static uchar*
devtreenode(Devtree *t, uchar *p, char *cp)
{
	uchar *e = (uchar*)t->stab;
	char *s;
	int n;

	while(p+4 <= e && beget4(p) == DtNop)
		p += 4;
	if(p+4 > e || beget4(p) != DtBeginNode)
		return nil;
	p += 4;
	if((s = memchr((char*)p, 0, e - p)) == nil)
		return nil;
	n = s - (char*)p;
	cp += n;
	if(cp >= &t->path[sizeof(t->path)])
		return nil;
	memmove(cp - n, (char*)p, n);
	*cp = 0;
	p += (n + 4) & ~3;
	while(p+12 <= e && beget4(p) == DtProp){
		n = beget4(p+4);
		if(p + 12 + n > e)
			return nil;
		s = t->stab + beget4(p+8);
		if(s < t->stab || s >= (char*)t->end
		|| memchr(s, 0, (char*)t->end - s) == nil)
			return nil;
		devtreeprop(t, s, p+12, n);
		p += 12 + ((n + 3) & ~3);
	}
	while(p+4 <= e && (beget4(p) == DtBeginNode || beget4(p) == DtNop)){
		if(beget4(p) == DtNop){
			p += 4;
			continue;
		}
		*cp = '/';
		p = devtreenode(t, p, cp+1);
		if(p == nil)
			return nil;
		*cp = 0;
	}
	if(p+4 > e || beget4(p) != DtEndNode)
		return nil;
	return p+4;
}

static void
devtreeconf(void)
{
	static Devtree t;
	uchar *base;
	ulong len;

	if((base = bootfdt(&len)) == nil)	/* checked by bootinfoinit() */
		return;
	t.end = base + beget4(base+4);
	t.stab = (char*)base + beget4(base+12);
	if(t.stab >= (char*)t.end || beget4(base+8) >= len)
		return;
	t.ncpu = 0;
	devtreenode(&t, base + beget4(base+8), t.path);
	if(t.ncpu > 0 && getconf("*ncpu") == nil){
		snprint(ncpubuf, sizeof ncpubuf, "%d", t.ncpu);
		addconf("*ncpu", ncpubuf);
	}
}

void
bootargsinit(void)
{
	int i, n;
	char *cp, *line[MAXCONF], *p, *q;

	/*
	 *  parse configuration args from dos file plan9.ini: the BootInfo
	 *  blob's config section (port/bootinfo.c), kept for good, as
	 *  confname[] and confval[] point into it
	 */
	cp = bootconfig();

	/*
	 * Strip out '\r', change '\t' -> ' '.
	 */
	p = cp;
	for(q = cp; *q; q++){
		if(*q == '\r')
			continue;
		if(*q == '\t')
			*q = ' ';
		*p++ = *q;
	}
	*p = 0;

	n = getfields(cp, line, MAXCONF, 1, "\n");
	for(i = 0; i < n; i++){
		if(*line[i] == '#')
			continue;
		cp = strchr(line[i], '=');
		if(cp == nil)
			continue;
		*cp++ = '\0';
		addconf(line[i], cp);
	}

	devtreeconf();

	/*
	 * BootInfo is the source of these; keep their historical text names
	 * for the code that reads them with getconf (archacpi.c, screen.c) and,
	 * through the environment, for user programs.
	 */
	if(bootinfo != nil){
		if(bootinfo->acpi != 0){
			snprint(acpibuf, sizeof acpibuf, "%#llux", bootinfo->acpi);
			addconf("*acpi", acpibuf);
		}
		if(bootinfo->fbbase != 0){
			snprint(fbbuf, sizeof fbbuf, "%udx%udx%udx%ud %s %#llux %#llux",
				bootinfo->fbwidth, bootinfo->fbheight, bootinfo->fbstride,
				bootinfo->fbdepth, bootinfo->fbchan, bootinfo->fbbase, bootinfo->fbsize);
			addconf("*bootscreen", fbbuf);
		}
	}
}

char*
getconf(char *name)
{
	int i;

	for(i = 0; i < nconf; i++)
		if(cistrcmp(confname[i], name) == 0)
			return confval[i];
	return 0;
}

void
setconfenv(void)
{
	int i;

	for(i = 0; i < nconf; i++){
		if(confname[i][0] != '*')
			ksetenv(confname[i], confval[i], 0);
		ksetenv(confname[i], confval[i], 1);
	}
}
