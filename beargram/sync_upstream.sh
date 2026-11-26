#!/usr/bin/env bash
# Merges the newest stable Telegram Desktop release (vX.Y.Z) into the current
# branch and resolves the conflicts that come up on every release:
#
# - core/version.h and winrc/Telegram.rc: the upstream version is taken and
#   beargram/branding.py puts the BearGram name back;
# - Telegram/CMakeLists.txt and dialogs/ui/dialogs_layout.cpp: the upstream
#   version is taken and the BearGram patch from beargram/patches is applied
#   on top of it;
# - Telegram/lib_ui: the submodule is not checked out in CI, so its gitlink
#   conflicts on every merge; the upstream commit is taken, BearGram tracks
#   upstream lib_ui and keeps no commits of its own there;
# - .github: BearGram keeps only its own workflows (beargram-*.yml), so the
#   upstream workflows and dependabot config are removed again.
#
# Any other conflict stops the script: it has to be resolved by hand.
#
# Prints "updated=<tag>" or "updated=" to $GITHUB_OUTPUT when it is set.
set -euo pipefail

UPSTREAM_URL="https://github.com/telegramdesktop/tdesktop.git"
VERSION_H="Telegram/SourceFiles/core/version.h"
TELEGRAM_RC="Telegram/Resources/winrc/Telegram.rc"
CMAKE_LISTS="Telegram/CMakeLists.txt"
DIALOGS_LAYOUT="Telegram/SourceFiles/dialogs/ui/dialogs_layout.cpp"
LIB_UI="Telegram/lib_ui"
PATCH_DIR="beargram/patches"

output() {
	if [ -n "${GITHUB_OUTPUT:-}" ]; then
		echo "$1" >> "$GITHUB_OUTPUT"
	fi
}

if git remote get-url upstream >/dev/null 2>&1; then
	git remote set-url upstream "$UPSTREAM_URL"
else
	git remote add upstream "$UPSTREAM_URL"
fi

latest=$(git ls-remote --tags --refs upstream 'v*' \
	| sed 's|.*refs/tags/||' \
	| grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' \
	| sort -V | tail -1)
current="v$(sed -n 's/^constexpr auto AppVersionStr = "\(.*\)";.*/\1/p' "$VERSION_H")"

echo "BearGram is on $current, the newest Telegram Desktop release is $latest."
newest=$(printf '%s\n%s\n' "$current" "$latest" | sort -V | tail -1)
if [ -z "$latest" ] || [ "$newest" = "$current" ]; then
	echo "Nothing to merge."
	output "updated="
	exit 0
fi

git fetch --no-tags upstream "refs/tags/$latest:refs/tags/$latest"

message="Merge Telegram Desktop $latest into BearGram"
if git merge --no-edit --no-ff -m "$message" "$latest"; then
	merged_cleanly=1
else
	merged_cleanly=0
fi

remove_upstream_github() {
	# Upstream workflows would run on every push to BearGram.
	local file
	for file in $(git ls-files .github/workflows .github/dependabot.yml); do
		case "$(basename "$file")" in
			beargram-*) ;;
			*) git rm -q --force "$file" ;;
		esac
	done
}

apply_beargram_patch() {
	# The upstream version is taken and the BearGram changes are reapplied
	# from the patch. Regenerate the patch on the BearGram branch whenever
	# one of these files changes on the BearGram side.
	local file="$1" patch="$PATCH_DIR/$2"
	git checkout --theirs -- "$file" || return 1
	if ! git apply --whitespace=nowarn "$patch" \
		&& ! git apply --ignore-whitespace --whitespace=nowarn "$patch"; then
		return 1
	fi
	git add -- "$file"
}

take_theirs_lib_ui() {
	# In CI the submodule worktree is not checked out, so git cannot merge
	# the gitlink and reports a conflict even for a fast-forward. Record
	# the upstream commit in the index directly.
	local theirs
	theirs=$(git ls-files -u -- "$LIB_UI" | awk '$3 == 3 { print $2; exit }')
	if [ -z "$theirs" ]; then
		theirs=$(git rev-parse "MERGE_HEAD:$LIB_UI") || return 1
	fi
	git update-index --cacheinfo "160000,$theirs,$LIB_UI"
}

if [ "$merged_cleanly" = 0 ]; then
	unresolved=()
	while IFS= read -r file; do
		case "$file" in
			"$VERSION_H"|"$TELEGRAM_RC")
				git checkout --theirs -- "$file"
				git add -- "$file"
				;;
			"$CMAKE_LISTS")
				apply_beargram_patch "$file" Telegram-CMakeLists.patch \
					|| unresolved+=("$file")
				;;
			"$DIALOGS_LAYOUT")
				apply_beargram_patch "$file" dialogs_layout.patch \
					|| unresolved+=("$file")
				;;
			"$LIB_UI")
				take_theirs_lib_ui || unresolved+=("$file")
				;;
			.github/workflows/*|.github/dependabot.yml)
				# Changed upstream, removed in BearGram: keep it removed.
				git rm -q --force -- "$file" 2>/dev/null || true
				;;
			*)
				unresolved+=("$file")
				;;
		esac
	done < <(git diff --name-only --diff-filter=U)
	if [ "${#unresolved[@]}" -ne 0 ]; then
		echo "::error::Merging $latest needs manual conflict resolution in:"
		printf '  %s\n' "${unresolved[@]}"
		git merge --abort
		exit 1
	fi
fi

remove_upstream_github
python3 beargram/branding.py
git add -- "$VERSION_H" "$TELEGRAM_RC"

if [ "$merged_cleanly" = 1 ]; then
	# The merge commit already exists: fold the fixes into it.
	git commit -q --amend --no-edit
else
	git commit -q --no-edit -m "$message"
fi

echo "Merged $latest."
output "updated=$latest"
