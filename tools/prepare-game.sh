#!/bin/bash
# A game from the builder's own disc, translated: tools/prepare-game.sh DISC.chd|DISC.iso WORK_DIR
# WORK_DIR (outside every repo) gets disc.iso, EBOOT.BIN, EBOOT.elf (decrypted), image.bin (rebased to 0x08804000) and gen/ (the
# translated C, game_funcs.h). Nothing here is ever committed: it is the game's code.
cd "$(dirname "$0")/.." || exit 1
SRC=${1:?disc}; W=${2:?work dir}
N=nakagawa-recomp; T=$N/build/brickprep
mkdir -p "$W/gen" "$T/chdr" || exit 1
tools/prepare-nakagawa.sh > /dev/null || exit 2
# the disc and decrypt tools, with plain gcc (no SDL3, no Windows)
if [ ! -x "$T/nk_decrypt" ] || [ ! -x "$T/chdcd2iso" ]; then
	L=$N/third_party/libchdr
	CF="-std=c99 -O2 -I$L/include -I$L/deps/lzma-26.02/include -I$L/deps/miniz-3.1.2 -I$L/deps/zstd-1.5.7 -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME"
	for f in $L/src/libchdr_{bitstream,cdrom,chd,codec_avhuff,codec_cdfl,codec_cdlz,codec_cdzl,codec_cdzs,codec_flac,codec_huff,codec_lzma,codec_zlib,codec_zstd,flac,huffman}.c \
			$L/deps/lzma-26.02/src/LzmaDec.c $L/deps/miniz-3.1.2/miniz.c $L/deps/zstd-1.5.7/zstddeclib.c; do
		gcc $CF -c "$f" -o "$T/chdr/$(basename "${f%.c}").o" || exit 3
	done
	ar rcs "$T/chdr/libnkchdr.a" "$T"/chdr/*.o
	gcc -O2 -std=c11 -D_GNU_SOURCE -I$N/src -I$N/src/core -I$N/src/rt $N/src/core/nk_decrypt_main.c $N/src/core/nk_psp_*.c \
		$N/src/core/nk_json.c $N/src/core/nk_shared_helpers.c -o "$T/nk_decrypt" || exit 3
	for t in chdcd2iso chdsha1; do
		gcc -O2 -I$L/include tools/$t.c "$T/chdr/libnkchdr.a" -lm -o "$T/$t" || exit 3
	done
fi
case "$SRC" in
	*.chd) "$T/chdsha1" "$SRC"; "$T/chdcd2iso" "$SRC" "$W/disc.iso" || exit 4 ;;
	*.iso) cp "$SRC" "$W/disc.iso" || exit 4 ;;
	*) echo "prepare-game: a .chd or an .iso"; exit 2 ;;
esac
python3 tools/psp_iso.py "$W/disc.iso" PSP_GAME/SYSDIR/EBOOT.BIN "$W/EBOOT.BIN" || exit 4
"$T/nk_decrypt" decrypt --in "$W/EBOOT.BIN" --out "$W/EBOOT.elf" || exit 5
( cd $N && python3 tools/prxload.py "$W/EBOOT.elf" 08804000 --psp-header="$W/EBOOT.BIN" --out="$W/image.bin" ) || exit 6
( cd $N && python3 tools/analyze.py "$W/EBOOT.elf" --base 08804000 --toml "$W/gen/functions.toml" --cfg-report "$W/gen/cfg.json" --quiet ) > "$W/gen/analyze.log" 2>&1 || exit 7
( cd $N && python3 tools/codegen.py "$W/EBOOT.elf" "$W/gen/game.c" --base=08804000 ) > "$W/gen/codegen.log" 2>&1 || exit 8
grep CODEGEN_COVERAGE "$W/gen/codegen.log"
# every translated function and resume entry, declared for every chunk
{ echo '#pragma once'; echo '#include "recomp.h"'; grep -h -o "^void [fr]_[0-9a-f]*(CpuState \*s)" "$W"/gen/game_*.c | sort -u | sed 's/$/;/'; } > "$W/gen/game_funcs.h"
echo "prepare-game: $(grep -c '^void' "$W/gen/game_funcs.h") functions in $W/gen"
