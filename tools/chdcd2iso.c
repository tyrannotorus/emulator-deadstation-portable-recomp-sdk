/* chdcd2iso: a PSP disc kept as a CD-type CHD (chdman createcd from the .iso) back to its .iso.
 * One data track, MODE1 (2048-byte sectors, stored first in each 2448-byte frame) or MODE1_RAW (2352, user data at 16).
 * Anything else is refused. Build against libchdr:
 *   cc -O2 -Ilibchdr/include chdcd2iso.c libnkchdr.a -lm -o chdcd2iso
 *   chdcd2iso IN.chd OUT.iso */
#include <libchdr/chd.h>
#include <libchdr/cdrom.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: chdcd2iso IN.chd OUT.iso\n");
		return 64;
	}
	chd_file *chd = NULL;
	chd_error err = chd_open(argv[1], CHD_OPEN_READ, NULL, &chd);
	if (err != CHDERR_NONE) {
		fprintf(stderr, "chdcd2iso: %s: %s\n", argv[1], chd_error_string(err));
		return 1;
	}
	const chd_header *h = chd_get_header(chd);
	char meta[256];
	uint32_t len = 0, tag = 0;
	uint8_t flags = 0;
	if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &len, &tag, &flags) != CHDERR_NONE) {
		fprintf(stderr, "chdcd2iso: no CD track metadata: not a CD-type CHD\n");
		return 1;
	}
	meta[len < sizeof(meta) ? len : sizeof(meta) - 1] = '\0';
	if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, 1, NULL, 0, NULL, NULL, NULL) == CHDERR_NONE) {
		fprintf(stderr, "chdcd2iso: more than one track: not a PSP disc image\n");
		return 1;
	}
	int track = 0, frames = 0, pregap = 0, postgap = 0;
	char type[32] = "", subtype[32] = "", pgtype[32] = "", pgsub[32] = "";
	if (sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
			&track, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap) < 4) {
		fprintf(stderr, "chdcd2iso: track metadata not understood: %s\n", meta);
		return 1;
	}
	int offset;
	if (strcmp(type, "MODE1") == 0)
		offset = 0;
	else if (strcmp(type, "MODE1_RAW") == 0)
		offset = 16;
	else {
		fprintf(stderr, "chdcd2iso: track type %s: only MODE1 or MODE1_RAW\n", type);
		return 1;
	}
	if (pregap != 0) {
		fprintf(stderr, "chdcd2iso: a pregap of %d frames: not expected for a .iso\n", pregap);
		return 1;
	}
	fprintf(stderr, "chdcd2iso: %s, %d frames of %u bytes, hunk %u bytes\n", meta, frames, h->unitbytes, h->hunkbytes);
	FILE *out = fopen(argv[2], "wb");
	if (out == NULL) {
		perror(argv[2]);
		return 1;
	}
	const uint32_t perHunk = h->hunkbytes / h->unitbytes;
	uint8_t *hunk = malloc(h->hunkbytes);
	uint32_t loaded = UINT32_MAX;
	for (int f = 0; f < frames; f++) {
		const uint32_t want = (uint32_t)f / perHunk;
		if (want != loaded) {
			err = chd_read(chd, want, hunk);
			if (err != CHDERR_NONE) {
				fprintf(stderr, "chdcd2iso: hunk %u: %s\n", want, chd_error_string(err));
				return 1;
			}
			loaded = want;
		}
		const uint8_t *frame = hunk + ((uint32_t)f % perHunk) * h->unitbytes;
		if (fwrite(frame + offset, 1, 2048, out) != 2048) {
			perror(argv[2]);
			return 1;
		}
	}
	if (fclose(out) != 0) {
		perror(argv[2]);
		return 1;
	}
	chd_close(chd);
	fprintf(stderr, "chdcd2iso: %d sectors, %lld bytes\n", frames, (long long)frames * 2048);
	return 0;
}
