#include "gc.h"

void
noretval(int n)
{
	USED(n);
}

/*
 *	addable:
 *		10	external, static
 *		11	auto, param
 *		12	register, memory at a register
 *		13	address of an external: a constant
 *		20	constant
 *	complex: registers needed, FNX with a call in it
 */
void
xcom(Node *n)
{
	Node *l, *r;
	int t;

	if(n == Z)
		return;
	l = n->left;
	r = n->right;
	n->complex = 0;
	n->addable = 0;
	switch(n->op) {
	case OCONST:
		n->addable = 20;
		return;

	case ONAME:
		n->addable = 10;
		if(n->class == CPARAM || n->class == CAUTO)
			n->addable = 11;
		return;

	case OREGISTER:
	case OINDREG:
		n->addable = 12;
		return;

	case OADDR:
		xcom(l);
		if(l->addable == 10)
			n->addable = 13;
		break;

	default:
		if(l != Z)
			xcom(l);
		if(r != Z)
			xcom(r);
		break;
	}
	if(n->op == OFUNC) {
		n->complex = FNX;
		return;
	}
	t = 0;
	if(l != Z)
		t = l->complex;
	if(r != Z) {
		if(r->complex == t && t != 0 && t < FNX)
			t++;
		else if(r->complex > t)
			t = r->complex;
	}
	if(t == 0)
		t = 1;
	n->complex = t;
}
