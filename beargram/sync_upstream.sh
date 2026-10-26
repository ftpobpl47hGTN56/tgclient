#!/usr/bin/env bash
# Merges the newest stable Telegram Desktop release (vX.Y.Z) into the current
# branch and resolves the conflicts that come up on every release:
#
# - core/version.h and winrc/Telegram.rc: the upstream version is taken and
#   beargram/branding.py puts the BearGram name back;
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

if [ "$merged_cleanly" = 0 ]; then
	unresolved=()
	for file in $(git diff --name-only --diff-filter=U); do
		case "$file" in
			"$VERSION_H"|"$TELEGRAM_RC")
				git checkout --theirs -- "$file"
				git add -- "$file"
				;;
			.github/workflows/*|.github/dependabot.yml)
				# Changed upstream, removed in BearGram: keep it removed.
				git rm -q --force -- "$file" 2>/dev/null || true
				;;
			*)
				unresolved+=("$file")
				;;
		esac
	done
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
