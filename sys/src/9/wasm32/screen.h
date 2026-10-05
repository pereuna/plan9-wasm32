/*
 * wasm32's screen and mouse (screen.c): a framebuffer in the kernel's
 * memory that the page shows (platform.js), the page's pointer as the
 * mouse, its cursor as the page's.  What port/devdraw.c and devmouse.c
 * want of the machine.
 */
typedef struct Cursor Cursor;
extern Cursor	cursor;
extern void	mousetrack(int, int, int, ulong);
extern void	absmousetrack(int, int, int, ulong);
extern Point	mousexy(void);
extern ulong	lastmousems(void);

/* screen.c */
extern void	mousectl(Cmdbuf*);
extern void	mouseresize(void);
extern void	mouseredraw(void);
extern Memdata*	attachscreen(Rectangle*, ulong*, int*, int*, int*);
extern void	flushmemscreen(Rectangle);
extern void	cursoron(void);
extern void	cursoroff(void);
extern void	setcursor(Cursor*);
extern void	blankscreen(int);
extern void	screeninit(void);
extern Memimage	*gscreen;

/* devdraw.c */
extern void	deletescreenimage(void);
extern void	resetscreenimage(void);
extern void	setscreenimageclipr(Rectangle);
extern void	drawflush(void);
extern QLock	drawlock;

/* no hardware drawing: every image is memory */
#define ishwimage(i)	0
