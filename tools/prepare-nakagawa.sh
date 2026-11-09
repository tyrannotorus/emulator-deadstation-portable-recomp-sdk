#!/bin/sh
# Nakagawa Recomp at this SDK's pin, with the SDK's patches (patches/nakagawa/*.patch, in order) over it. Run after cloning or
# updating the submodule, and again any time: the patched tree is made in a temporary copy of the pin, and only the files whose
# content differs are written into the submodule (the others keep their times, so nothing rebuilds for nothing). A file the patches
# don't touch is put back as the pin has it. A patch that doesn't apply stops here, the submodule untouched.
cd "$(dirname "$0")/.." || exit 1
N=nakagawa-recomp
PIN=$(cat nakagawa.pin)
[ "$(git -C $N rev-parse HEAD)" = "$PIN" ] || { echo "prepare-nakagawa: $N is not at the pin $PIN"; exit 2; }
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "${TMP:?}"' EXIT
git -C $N archive "$PIN" | tar -x -C "$TMP" || { echo "prepare-nakagawa: the pin can't be copied"; exit 3; }
for p in patches/nakagawa/*.patch; do
	(cd "$TMP" && git apply "$OLDPWD/$p") || { echo "prepare-nakagawa: $(basename "$p") does not apply"; exit 3; }
	echo "patch $(basename "$p")"
done
# the files the patches change, and the files the submodule has changed: each made as the patched copy has it
for f in $(cat patches/nakagawa/*.patch | sed -n 's|^+++ b/||p' | sort -u) $(git -C $N diff --name-only); do
	if [ -f "$TMP/$f" ]; then
		cmp -s "$TMP/$f" "$N/$f" || { cp "$TMP/$f" "$N/$f" && echo "written: $f"; }
	else
		git -C $N checkout -q -- "$f"
	fi
done
exit 0
