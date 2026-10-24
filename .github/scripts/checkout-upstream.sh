#!/usr/bin/env bash
# Fetch the exact resolved public upstream commit, even if its branch moved.
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: checkout-upstream.sh <repository> <commit-sha> <fresh-directory>" >&2
  exit 2
fi
repository="$1"
revision="$2"
destination="$3"
case "$repository" in
  topo-core|topo-lang|topo-lang-cpp|topo-lang-rust|topo-cli) ;;
  *) echo "unexpected upstream repository: $repository" >&2; exit 2 ;;
esac
if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
  echo "upstream checkout requires a full resolved commit SHA" >&2
  exit 2
fi
if [ -e "$destination" ] || [ -L "$destination" ]; then
  echo "upstream checkout requires a fresh directory: $destination" >&2
  exit 2
fi

echo "Checking out public repository Topo-lang/$repository at $revision"
git init --quiet "$destination"
git -C "$destination" remote add origin "https://github.com/Topo-lang/$repository.git"
git -C "$destination" fetch --depth 1 origin "$revision"
git -C "$destination" checkout --quiet --detach FETCH_HEAD
actual="$(git -C "$destination" rev-parse HEAD)"
if [ "$actual" != "$revision" ]; then
  echo "upstream identity mismatch: expected $revision, got $actual" >&2
  exit 1
fi
echo "Verified Topo-lang/$repository HEAD=$actual"
