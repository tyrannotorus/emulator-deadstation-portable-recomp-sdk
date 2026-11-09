/* chdsha1: the SHA-1 a CHD's header records for its data (what a module's manifest names its disc by). */
#include <libchdr/chd.h>
#include <stdio.h>
int main(int argc, char **argv)
{
	chd_file *chd = NULL;
	if (argc != 2 || chd_open(argv[1], CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE)
		return fprintf(stderr, "usage: chdsha1 FILE.chd\n"), 1;
	const chd_header *h = chd_get_header(chd);
	for (int i = 0; i < 20; i++)
		printf("%02x", h->sha1[i]);
	printf("  (version %u)\n", h->version);
	chd_close(chd);
	return 0;
}
