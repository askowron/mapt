/* Differential test: our dpkg_vercmp() must agree with the reference
 * implementation, "dpkg --compare-versions", on a pile of real world
 * version strings.  Skipped when dpkg is not installed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vercmp.h"

static const char *const versions[] = {
	"0",
	"1",
	"1.0",
	"1.00",
	"1.0.1",
	"1.0.10",
	"1.0a",
	"1.0~",
	"1.0~~",
	"1.0~rc1",
	"1.0~rc10",
	"1.0-1",
	"1.0-2",
	"1.0-1ubuntu1",
	"1.0-1ubuntu1.1",
	"0.9.9",
	"1:1.0",
	"2:0.1",
	"1:2.0-1",
	"007",
	"7",
	"1.2.3-1",
	"1.2.3-2",
	"2.0.10",
	"2.0.9",
	"1.4.2",
	"9.9.9",
	"1.0+git20230101",
	"1.0+git20230102",
	"5.2.21-2ubuntu3",
	"5.2.21-2ubuntu3.1",
	"3.3.9-13",
	"1:3.3.9-13",
	"20240101",
	"20240101+dfsg",
	"0.9-1+b2",
	"0.9-1",
	"1.0.",
	"1.0a1",
	"1.0A1",
	"1.0-0",
	"1.0-0.1"
};

#define N ((int)(sizeof(versions) / sizeof(versions[0])))

/* Ask dpkg for the sign of a - b: -1, 0 or 1.  Returns 99 on failure. */
static int dpkg_sign(const char *a, const char *b)
{
	char cmd[1024];
	int gt, lt;

	snprintf(cmd, sizeof(cmd),
		 "dpkg --compare-versions '%s' gt '%s'", a, b);
	gt = system(cmd) == 0;
	snprintf(cmd, sizeof(cmd),
		 "dpkg --compare-versions '%s' lt '%s'", a, b);
	lt = system(cmd) == 0;

	if (gt && !lt)
		return 1;
	if (lt && !gt)
		return -1;
	if (!gt && !lt)
		return 0;
	return 99;
}

int main(void)
{
	int i, j, checked = 0, failures = 0;

	if (system("command -v dpkg >/dev/null 2>&1") != 0) {
		printf("dpkg not available, skipping differential test\n");
		return 0;
	}

	for (i = 0; i < N; i++) {
		for (j = 0; j < N; j++) {
			int want = dpkg_sign(versions[i], versions[j]);
			int got;

			if (want == 99) {
				fprintf(stderr, "dpkg failed on %s vs %s\n",
					versions[i], versions[j]);
				failures++;
				continue;
			}
			got = dpkg_vercmp(versions[i], versions[j]);
			got = got < 0 ? -1 : got > 0 ? 1 : 0;
			checked++;
			if (got != want) {
				fprintf(stderr,
					"FAIL: vercmp(\"%s\", \"%s\") = %d, "
					"dpkg says %d\n",
					versions[i], versions[j], got, want);
				failures++;
				if (failures > 20)
					return 1;
			}
		}
	}

	if (failures) {
		fprintf(stderr, "%d of %d comparisons differ from dpkg\n",
			failures, checked);
		return 1;
	}
	printf("vercmp matches dpkg on %d comparisons\n", checked);
	return 0;
}
