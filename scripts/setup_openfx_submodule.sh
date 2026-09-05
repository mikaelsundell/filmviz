#!/bin/sh
set -eu

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$repo_root"

url="https://github.com/AcademySoftwareFoundation/openfx.git"
path="external/openfx"

if git ls-files --stage "$path" 2>/dev/null | grep -q '^160000 '; then
    echo "OpenFX submodule is already registered."
    git submodule update --init --recursive "$path"
    exit 0
fi

if [ -e "$path" ]; then
    echo "Cannot add OpenFX submodule because this path already exists:"
    echo "  $path"
    echo "Move or remove it, then run this script again."
    exit 1
fi

# Remove the pre-supplied .gitmodules entry temporarily if necessary so
# 'git submodule add' can create the gitlink cleanly.
if [ -f .gitmodules ] && grep -q 'path = external/openfx' .gitmodules; then
    cp .gitmodules .gitmodules.filmviz-backup
    python3 - <<'PY'
from pathlib import Path
p = Path(".gitmodules")
text = p.read_text()
block = '[submodule "external/openfx"]\n\tpath = external/openfx\n\turl = https://github.com/AcademySoftwareFoundation/openfx.git\n'
text = text.replace(block, "")
p.write_text(text)
PY
fi

git submodule add "$url" "$path"

if [ -f .gitmodules.filmviz-backup ]; then
    rm -f .gitmodules.filmviz-backup
fi

git submodule update --init --recursive "$path"

echo
echo "OpenFX submodule ready:"
echo "  $path"
echo
echo "Commit the submodule registration with:"
echo "  git add .gitmodules $path"
