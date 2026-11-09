/* The game's memory image from the player's own disc, at a module's first run: the disc's EBOOT.BIN decrypted (Nakagawa's PSP
 * crypto, src/core), its segments laid out flat from 0 (the bss zeroed) and its type-A relocations applied for the load base, as
 * tools/prxload.py does (a port of PPSSPP's ElfReader::LoadRelocations). A module ships no game data: this is made on the Brick and
 * checked against the SHA-256 its module.cfg names. */
#include "nk_iso.h"
#include "nk_psp_crypto.h"
#include "nk_psp_container.h"
#include "nk_psp_keystore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto out; } while (0)

/* a file's SHA-256 as 64 hex digits: 1, or 0 when it can't be read (the runtime's SHA-256, vfpu_tables.c) */
void sr_vfpu_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);
int sdk_file_sha256(const char *path, char hex[65])
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
	const int ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	if (!ok) {
		free(buf);
		return 0;
	}
	uint8_t d[32];
	sr_vfpu_sha256(buf, (size_t)n, d);
	free(buf);
	for (int i = 0; i < 32; i++)
		snprintf(hex + i * 2, 3, "%02x", d[i]);
	return 1;
}

/* the image for load address `base`, to `out_path`: 1, or 0 with why in err */
int sdk_build_image(const char *disc, const char *out_path, uint32_t base, char *err, size_t errn)
{
	int ok = 0;
	NkIsoReader *iso = NULL;
	uint8_t *eboot = NULL, *plain = NULL, *img = NULL;
	size_t plain_size = 0;
	NkKeystore *ks = NULL;

	iso = nk_iso_reader_open(disc);
	if (!iso)
		FAIL("the disc %s can't be read", disc);
	uint32_t lba = 0, size = 0;
	bool dir = false;
	if (nk_iso_reader_lookup(iso, "PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &size, &dir) != 0 || dir || size < 0x100)
		FAIL("the disc has no PSP_GAME/SYSDIR/EBOOT.BIN");
	eboot = malloc(size);
	if (!eboot || nk_iso_reader_read(iso, lba, 0, eboot, size) != (int)size)
		FAIL("EBOOT.BIN can't be read from the disc");

	/* decrypted with the built-in tables */
	ks = nk_keystore_create();
	char kerr[256];
	if (!ks || nk_keystore_load_builtin(ks, kerr, sizeof(kerr)) != NK_PSP_OK)
		FAIL("no keys: %s", ks ? kerr : "out of memory");
	NkPspCtx ctx;
	nk_psp_ctx_init(&ctx, ks);
	const int rc = nk_container_decrypt(&ctx, eboot, size, &plain, &plain_size);
	if (rc != NK_PSP_OK)
		FAIL("EBOOT.BIN can't be decrypted (%d)", rc);
	const uint8_t *e = plain;
	if (plain_size < 52 || memcmp(e, "\x7f" "ELF", 4) != 0)
		FAIL("the decrypted EBOOT.BIN is not an ELF");

	/* the segments, flat from 0; their in-memory sizes from the ~PSP header where the ELF lost its bss */
	const uint32_t phoff = rd32(e + 28), shoff = rd32(e + 32);
	const uint16_t phentsize = rd16(e + 42), phnum = rd16(e + 44), shentsize = rd16(e + 46), shnum = rd16(e + 48);
	uint32_t seg_vaddr[8], nseg = 0, extent = 0;
	const uint32_t hdr_segs = eboot[0x27];
	for (uint16_t i = 0; i < phnum && nseg < 8; i++) {
		const uint8_t *ph = e + phoff + (uint32_t)i * phentsize;
		if ((size_t)(ph - e) + 32 > plain_size || rd32(ph) != 1u)
			continue;
		const uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16);
		uint32_t memsz = rd32(ph + 20);
		if (nseg < hdr_segs && nseg < 4 && rd32(eboot + 0x54 + 4 * nseg) > memsz)
			memsz = rd32(eboot + 0x54 + 4 * nseg);
		if ((uint64_t)off + filesz > plain_size || filesz > memsz || vaddr > 0x04000000u || memsz > 0x04000000u)
			FAIL("segment %u is out of range", nseg);
		if (vaddr + memsz > extent)
			extent = vaddr + memsz;
		seg_vaddr[nseg++] = vaddr;
	}
	if (nseg == 0 || extent == 0)
		FAIL("no loadable segment");
	img = calloc(1, extent);
	if (!img)
		FAIL("out of memory");
	for (uint16_t i = 0, s = 0; i < phnum; i++) {
		const uint8_t *ph = e + phoff + (uint32_t)i * phentsize;
		if ((size_t)(ph - e) + 32 > plain_size || rd32(ph) != 1u)
			continue;
		memcpy(img + rd32(ph + 8), e + rd32(ph + 4), rd32(ph + 16));
		if (++s == nseg)
			break;
	}

	/* type-A relocations (section type 0x700000A0); a packed table (0x700000A1) isn't ported: said, not skipped */
	unsigned applied = 0;
	for (uint16_t i = 0; i < shnum; i++) {
		const uint8_t *sh = e + shoff + (uint32_t)i * shentsize;
		if ((size_t)(sh - e) + 40 > plain_size)
			FAIL("section header %u is out of range", i);
		const uint32_t type = rd32(sh + 4), off = rd32(sh + 16), ssize = rd32(sh + 20);
		if (type == 0x700000A1u)
			FAIL("packed relocations (type 0x700000A1) aren't supported yet");
		if (type != 0x700000A0u)
			continue;
		if (ssize % 8u || (uint64_t)off + ssize > plain_size)
			FAIL("relocation table %u is out of range", i);
		const uint32_t n = ssize / 8u;
		const uint8_t *rel = e + off;
		for (uint32_t r = 0; r < n; r++) {
			const uint32_t offset = rd32(rel + r * 8u), info = rd32(rel + r * 8u + 4u);
			const uint32_t type_r = info & 0xFu, ofs_seg = (info >> 8) & 0xFFu, addr_seg = (info >> 16) & 0xFFu;
			if (ofs_seg >= nseg || addr_seg >= nseg)
				FAIL("relocation %u: segment out of range", r);
			const uint32_t addr = offset + seg_vaddr[ofs_seg], to = base + seg_vaddr[addr_seg];
			if ((uint64_t)addr + 4 > extent)
				FAIL("relocation %u: address 0x%08x out of the image", r, addr);
			uint32_t op = rd32(img + addr);
			switch (type_r) {
			case 0: case 7: case 8: continue;	/* NONE, GPREL16, LITERAL: nothing */
			case 2: op += to; break;	/* R_MIPS_32 */
			case 4: op = (op & 0xFC000000u) | (((op & 0x03FFFFFFu) + (to >> 2)) & 0x03FFFFFFu); break;	/* R_MIPS_26 */
			case 5: {	/* R_MIPS_HI16, with the LO16 that follows it */
				uint32_t cur = (op & 0xFFFFu) << 16;
				uint32_t t = r + 1;
				for (; t < n; t++) {
					const uint32_t tt = rd32(rel + t * 8u + 4u) & 0xFu;
					if (tt == 5u || (tt != 6u && tt != 1u))
						continue;
					const uint32_t tinfo = rd32(rel + t * 8u + 4u), tseg = (tinfo >> 8) & 0xFFu;
					if (tseg >= nseg)
						FAIL("relocation %u: segment out of range", t);
					const uint32_t lo_addr = rd32(rel + t * 8u) + seg_vaddr[tseg];
					if ((uint64_t)lo_addr + 4 > extent)
						FAIL("relocation %u: address out of the image", t);
					const uint32_t lo = rd32(img + lo_addr) & 0xFFFFu;
					cur = cur + (uint32_t)(int32_t)(int16_t)lo + to;
					break;
				}
				if (t == n)
					cur += to;
				op = (op & 0xFFFF0000u) | (((cur >> 16) + ((cur & 0x8000u) ? 1u : 0u)) & 0xFFFFu);
				break;
			}
			case 6: case 1: op = (op & 0xFFFF0000u) | (((op & 0xFFFFu) + to) & 0xFFFFu); break;	/* LO16, 16 */
			default: FAIL("relocation %u: type %u isn't supported", r, type_r);
			}
			wr32(img + addr, op);
			applied++;
		}
	}
	FILE *f = fopen(out_path, "wb");
	if (!f || fwrite(img, 1, extent, f) != extent || fclose(f) != 0)
		FAIL("%s can't be written", out_path);
	fprintf(stderr, "image: %u bytes, %u relocations, from %s\n", extent, applied, disc);
	ok = 1;
out:
	free(img);
	free(plain);
	free(eboot);
	if (ks)
		nk_keystore_free(ks);
	if (iso)
		nk_iso_reader_close(iso);
	return ok;
}
