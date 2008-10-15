unsigned a;
unsigned b;
unsigned c;
unsigned d;

unsigned *block1, *block2, *block3, *block4, *block5, *block6;
unsigned gi1, gi2, gi3, gi4, gi5, gi6, gi7, gi8, gi9, gi10, gi11, gi12;
unsigned *gp10, *gp11, *gp12, *gp13, *gp14, *gp15, *gp16, *gp17, *gp18;
unsigned use;
volatile unsigned volatile_use;
unsigned **volatile_p;

int main(int argc, char **argv)
{
	return 0;
}

unsigned k3_3(char* base, unsigned i1, unsigned i2, unsigned i3, unsigned k1, unsigned k2, unsigned k3)
{
	char a1, a2, a3;
	char b1, b2, b3;
	char c1, c2, c3;

	a1 = i1 + k1;
	a2 = base[i2 + k1];
	a3 = base[i3 + k1];

	b1 = i1 + k2;
	b2 = base[i2 + k2];
	b3 = base[i3 + k2];

	c1 = i1 + k3;
	c2 = base[i2 + k3];
	c3 = base[i3 + k3];

	if (a1 != a2)
		return a3;
	if (b1 != b2)
		return b3;
	if (c1 != c2)
		return c3;

	return 0;
}

unsigned k3_3_am(char* base, unsigned i1, unsigned i2, unsigned i3, unsigned k1, unsigned k2, unsigned k3)
{
	char a1, a2, a3;
	char b1, b2, b3;
	char c1, c2, c3;

	a1 = base[i1 + k1];
	a2 = base[i2 + k1];
	a3 = base[i3 + k1];

	b1 = base[i1 + k2];
	b2 = base[i2 + k2];
	b3 = base[i3 + k2];

	c1 = base[i1 + k3];
	c2 = base[i2 + k3];
	c3 = base[i3 + k3];

	if (a1 != a2)
		return a3;
	if (b1 != b2)
		return b3;
	if (c1 != c2)
		return c3;

	return 0;
}

unsigned k3_3_2(char* base, int i1, int i2, int i3, int k1, int k2, int k3)
{
	char a1, a2, a3;
	char b1, b2, b3;
	char c1, c2, c3;

	a1 = base[i1 + k1];
	a2 = base[i2 + k1];
	a3 = base[i3 + k1];

	b1 = base[i1 + k2];
	b2 = base[i2 + k2];
	b3 = base[i3 + k2];

	c1 = base[i1 + k3];
	c2 = base[i2 + k3];
	c3 = base[i3 + k3];

	if (a1 != a2)
		return a3;
	if (b1 != b2)
		return b3;
	if (c1 != c2)
		return c3;

	return 0;
}

unsigned k3_3_3(char* base, unsigned i1, unsigned i2, unsigned i3, unsigned k1, unsigned k2, unsigned k3)
{
	volatile_use = i1 + k1;
	volatile_use = i2 + k1;
	volatile_use = i3 + k1;

	volatile_use = i1 + k2;
	volatile_use = i2 + k2;
	volatile_use = i3 + k2;

	volatile_use = i1 + k3;
	volatile_use = i2 + k3;
	volatile_use = i3 + k3;

	return 0;
}

unsigned k3_3_4(char* base, unsigned i1, unsigned i2, unsigned i3, char *k1, char *k2, char *k3)
{
	gp10 = i1 + k1;
	gp11 = i2 + k1;
	gp12 = i3 + k1;

	gp13 = i1 + k2;
	gp14 = i2 + k2;
	gp15 = i3 + k2;

	gp16 = i1 + k3;
	gp17 = i2 + k3;
	gp18 = i3 + k3;

	return 0;
}

void diamond(void)
{
	unsigned as  = a << 3;
	unsigned asb = as + 123235;
	unsigned asc = as + 235346;
	unsigned sum = asb + asc;

	d = sum;

	use = k3_3_4(block1, asb, gi1, gi2, &gi3, &gi4, &gi5);
//	use = k3_3(block2, asb, gi3, gi4, 13, 14, 15);
//
//	use = k3_3(block3, asb, gi5, gi6, 16, 17, 18);
//	use = k3_3(block4, asb, gi7, gi8, 19, 20, 21);
//
//	use = k3_3(block5, asb, gi9, gi10, 22, 23, 24);
//	use = k3_3(block6, asb, gi11, gi12, 25, 26, 27);
}
