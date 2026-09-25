#include <stdio.h>
#include <string.h>

#include "vercmp.h"

static int failures;

static void expect(const char *a, const char *b, int want)
{
	int got = dpkg_vercmp(a, b);
	int norm = got < 0 ? -1 : got > 0 ? 1 : 0;

	if (norm != want) {
		fprintf(stderr, "FAIL: dpkg_vercmp(\"%s\", \"%s\") = %d, "
				"expected %d\n",
			a, b, got, want);
		failures++;
	}
}

static void same(const char *v)
{
	expect(v, v, 0);
}

int main(void)
{
	/* equality */
	same("");
	same("1.0");
	same("1:1.0");
	same("1.0~rc1");

	/* plain ordering, taken from the dpkg documentation */
	expect("1.0", "2.0", -1);
	expect("2.0", "1.0", 1);
	expect("1.0", "1.0", 0);

	/* epochs */
	expect("1:1.0", "2.0", 1);
	expect("2.0", "1:1.0", -1);
	expect("1:1.0", "1:2.0", -1);

	/* leading zeros are ignored */
	expect("1.0", "1.00", 0);
	expect("007", "7", 0);

	/* tilde sorts before everything, including "nothing" */
	expect("1.0~rc1", "1.0", -1);
	expect("1.0", "1.0~rc1", 1);
	/* ... and the more tildes, the smaller the version */
	expect("1.0~~", "1.0~", -1);

	/* letters sort after the end of string but before punctuation */
	expect("1.0", "1.0a", -1);
	expect("1.0a", "1.0", 1);
	expect("1.0", "1.0.", -1);

	/* inside a digit run a number always beats a letter run */
	expect("1.01", "1.0a", 1);
	expect("1.0.1", "1.0.10", -1);

	/* numeric parts are compared numerically, not lexically */
	expect("1.10", "1.9", 1);
	expect("2.0.10", "2.0.9", 1);

	/* real world Ubuntu style versions */
	expect("5.2.21-2ubuntu3", "5.2.21-2ubuntu3.1", -1);
	expect("1.2.3-1", "1.2.3-2", -1);
	expect("2:1.4.2-3", "1:9.9.9", 1);
	expect("0.9+git20230101-1", "0.9+git20230101-2", -1);

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	printf("all vercmp tests passed\n");
	return 0;
}
