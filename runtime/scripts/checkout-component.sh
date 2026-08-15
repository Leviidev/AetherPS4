#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <name> <https-url> <40-hex-revision>" >&2
  exit 64
fi

name=$1
url=$2
revision=$3
[[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "unsafe component name" >&2; exit 64; }
[[ "$url" =~ ^https:// ]] || { echo "component URL must use HTTPS" >&2; exit 64; }
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo "revision must be 40 lowercase hex characters" >&2; exit 64; }

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
dest="$project_root/runtime/sources/$name"
mkdir -p "$(dirname "$dest")"

if [[ ! -d "$dest/.git" ]]; then
  git clone --filter=blob:none --no-checkout "$url" "$dest"
elif [[ "$(git -C "$dest" remote get-url origin)" != "$url" ]]; then
  echo "origin URL mismatch for $name" >&2
  exit 1
fi

if ! git -C "$dest" cat-file -e "$revision^{commit}" >/dev/null 2>&1; then
  git -C "$dest" fetch --depth 1 origin "$revision"
fi
git -C "$dest" checkout --detach --force "$revision"
git -C "$dest" clean -fd
test "$(git -C "$dest" rev-parse HEAD)" = "$revision"
test -z "$(git -C "$dest" status --porcelain)"
printf '%s %s\n' "$name" "$revision"
