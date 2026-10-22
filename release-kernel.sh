#!/bin/bash
# release-kernel.sh - Create signed Devario kernel release tags in Seafoam-Labs/linux
#
# Usage: ./release-kernel.sh [options] <kernel-version> [pkgrel]
#   ./release-kernel.sh 6.19.3              -> devario-6.19.3-1
#   ./release-kernel.sh 6.19.3 2            -> devario-6.19.3-2
#   ./release-kernel.sh --dev 7.2.3         -> devario-dev-7.2.3-1
#   ./release-kernel.sh --scx 7.2.3         -> devario-scx-7.2.3-1
#   ./release-kernel.sh --dry-run 6.19-rc8

set -euo pipefail

readonly SCRIPT_NAME=${0##*/}
# The only repository releases may be published to. RELEASE_REPO_SLUG in the
# environment overrides it (for test setups only).
readonly EXPECTED_REPO_SLUG=${RELEASE_REPO_SLUG:-Seafoam-Labs/linux}
# Release channel: "" (stable), "dev" or "scx"; set by --dev / --scx.
# Selects the tag prefix (devario[-<channel>]) and the release title.
CHANNEL=""
TAG_PREFIX="devario"
RELEASE_TITLE="Devario Linux"

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

if [[ -t 1 ]]; then
    C_BOLD=$'\e[1m' C_DIM=$'\e[2m' C_RED=$'\e[31m' C_GREEN=$'\e[32m'
    C_YELLOW=$'\e[33m' C_BLUE=$'\e[34m' C_RESET=$'\e[0m'
else
    C_BOLD='' C_DIM='' C_RED='' C_GREEN='' C_YELLOW='' C_BLUE='' C_RESET=''
fi

step() { printf '%s==>%s %s%s%s\n' "$C_BLUE" "$C_RESET" "$C_BOLD" "$*" "$C_RESET"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '%s  ✔%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn() { printf '%s  !%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die()  { printf '%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

confirm() {
    # confirm <prompt>  -> returns 0 on yes
    local answer
    [[ $ASSUME_YES == 1 ]] && return 0
    read -rp "$1 [y/N] " answer
    [[ $answer == [yY] ]]
}

usage() {
    cat <<EOF
Usage: ${SCRIPT_NAME} [options] <kernel-version> [pkgrel]

Create a signed ${TAG_PREFIX}-<version>-<pkgrel> tag on the current HEAD,
then optionally push it and publish a GitHub release with a signed tarball.

  Kernel variant          Tag
  ─────────────────────────────────────────
  Stable initial          devario-6.19.0-1
  Stable point release    devario-6.19.3-1
  Stable re-spin          devario-6.19.3-2
  LTS                     devario-6.18.9-1
  RC                      devario-6.19-rc8-1
  Dev build (--dev)       devario-dev-7.2.3-1
  scx kernel (--scx)      devario-scx-7.2.3-1

Each channel (devario-*, devario-dev-*, devario-scx-*) is a separate line
of tags: "previous release" and the changelog diff are computed only
within the same channel.

Arguments:
  kernel-version   X.Y.Z (6.19.3) or X.Y-rcN (6.19-rc8)
  pkgrel           Package release number (default: 1)

Options:
  -d, --dev        Dev channel: tag devario-dev-<version>-<pkgrel>, release
                   titled "Devario Linux (dev) <version>-<pkgrel>"
  -s, --scx        scx channel: tag devario-scx-<version>-<pkgrel>, release
                   titled "Devario Linux (scx) <version>-<pkgrel>"
                   (--dev and --scx are mutually exclusive)
  -n, --dry-run    Only print the generated changelog, change nothing
  -y, --yes        Do not ask for confirmation
  -r, --resume     Publish an already existing local tag: verify its
                   signature, push it if origin lacks it (a *different* tag
                   of the same name on origin aborts), create the GitHub
                   release if missing or re-upload tarball + signature to
                   an existing one. The tag is never recreated or moved;
                   release notes come from its message. An existing
                   release's title/notes are not touched.
  -h, --help       Show this help

Checks performed before tagging (all fatal):
  - Makefile at HEAD declares exactly <kernel-version>
  - the upstream "Linux <kernel-version>" commit is on the first-parent
    line of HEAD (merging upstream in as a side branch is not a base) and
    no newer "Linux X.Y.*" commit sits above it
  - every ${TAG_PREFIX}-* tag on origin is present locally (fetch first)
  - the previous ${TAG_PREFIX}-* tag of the channel has its own upstream commit
  Uncommitted changes to tracked files only produce a warning.

Publishing:
  - only to ${EXPECTED_REPO_SLUG} (origin must point there)
  - dev/scx channels and -rc versions are created as GitHub pre-releases

Recovering a half-finished publish:
  If the script fails after the tag was created (push, signing or
  'gh release' error), fix the cause and re-run with --resume:
      ${SCRIPT_NAME} [--dev|--scx] --resume <kernel-version> [pkgrel]

Requirements:
  - A GPG key configured via 'git config user.signingkey' for the tag.
    The tarball is always signed with the key that signed the tag (also
    on --resume), so tag and tarball never carry different signers; if
    that key's secret part is unavailable, publishing stops.
  - 'gh' authenticated against the origin repository
  - Topic branches merged (not squashed) into the release branch; the
    changelog is built from first-parent merge commits on top of the
    upstream "Linux X.Y.Z" commit
EOF
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

DRY_RUN=0
ASSUME_YES=0
RESUME=0
POSITIONAL=()

set_channel() {
    [[ -z $CHANNEL || $CHANNEL == "$1" ]] || die "--dev and --scx are mutually exclusive"
    CHANNEL=$1
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--dev)     set_channel dev ;;
        -s|--scx)     set_channel scx ;;
        -n|--dry-run) DRY_RUN=1 ;;
        -y|--yes)     ASSUME_YES=1 ;;
        -r|--resume)  RESUME=1 ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; POSITIONAL+=("$@"); break ;;
        -*)           die "unknown option: $1" ;;
        *)            POSITIONAL+=("$1") ;;
    esac
    shift
done

(( ${#POSITIONAL[@]} <= 2 )) \
    || die "too many arguments: ${POSITIONAL[*]:2}"
VERSION=${POSITIONAL[0]:-}
PKGREL=${POSITIONAL[1]:-1}

if [[ -n $CHANNEL ]]; then
    TAG_PREFIX="devario-${CHANNEL}"
    RELEASE_TITLE="Devario Linux (${CHANNEL})"
fi
readonly CHANNEL TAG_PREFIX RELEASE_TITLE
# Only stable, non-rc releases are "latest" on GitHub.
PRERELEASE=''
[[ -n $CHANNEL || $VERSION == *-rc* ]] && PRERELEASE=1
readonly PRERELEASE

[[ -n $VERSION ]] || { usage >&2; exit 1; }
[[ $VERSION =~ ^[0-9]+\.[0-9]+(\.[0-9]+|-rc[0-9]+)$ ]] \
    || die "version must be X.Y.Z or X.Y-rcN (got: $VERSION)"
[[ $PKGREL =~ ^[1-9][0-9]*$ ]] \
    || die "pkgrel must be a positive integer (got: $PKGREL)"

git rev-parse --is-inside-work-tree &>/dev/null \
    || die "not inside a git repository"

TAG="${TAG_PREFIX}-${VERSION}-${PKGREL}"
TITLE="${RELEASE_TITLE} ${VERSION}-${PKGREL}"
if [[ $VERSION == *-rc* ]]; then
    MAJOR_MINOR=${VERSION%-rc*}      # 6.19-rc8 -> 6.19
else
    MAJOR_MINOR=${VERSION%.*}        # 6.19.3   -> 6.19
fi

[[ $DRY_RUN == 1 && $RESUME == 1 ]] && die "--dry-run and --resume are mutually exclusive"

if git rev-parse -q --verify "refs/tags/${TAG}" &>/dev/null; then
    [[ $RESUME == 1 ]] || die "tag '${TAG}' already exists (use --resume to finish publishing it)"
else
    [[ $RESUME == 0 ]] || die "tag '${TAG}' does not exist; nothing to resume"
fi

# ---------------------------------------------------------------------------
# Git queries
# ---------------------------------------------------------------------------

# Hash of the upstream "Linux <version>" commit on the *first-parent* line
# of <ref>. The changelog walks first-parent merges on top of this commit,
# so an upstream release that was merged in as a side branch (instead of
# being the base the topic branches sit on) is deliberately not accepted.
# Upstream tags the first release of a series as "Linux X.Y" (no ".0").
upstream_commit() {
    local version=$1 ref=$2 hash
    hash=$(git log --first-parent --format='%H %s' "$ref" \
        | awk -v want="Linux ${version}" '{ h=$1; $1=""; sub(/^ /, "")
                                           if ($0 == want) { print h; exit } }')
    if [[ -z $hash && $version == *.0 ]]; then
        upstream_commit "${version%.0}" "$ref"
        return
    fi
    printf '%s' "$hash"
}

# Newest devario-<major.minor>-* tag that sorts *before* the tag being
# created (full version incl. pkgrel; rc sorts before .0). Ancestry is
# deliberately ignored so this survives stable rebases, and a re-spin of an
# older point release (6.19.3-2 after 6.19.7-1 exists) picks 6.19.3-1.
previous_tag() {
    { git tag -l "${TAG_PREFIX}-${1}.*" "${TAG_PREFIX}-${1}-rc*"; printf '%s\n' "$TAG"; } \
        | LC_ALL=C sort -uV \
        | awk -v tag="$TAG" '$0 == tag { print prev; exit } { prev = $0 }'
}

# Fingerprint of the OpenPGP key that signed tag <tag>; empty if the
# signature is missing or invalid.
tag_signing_key() {
    git -c gpg.format=openpgp verify-tag --raw "$1" 2>&1 \
        | awk '$1 == "[GNUPG:]" && $2 == "VALIDSIG" { print $3; exit }'
}

# Kernel version as declared by the Makefile at <ref>, normalised to the
# X.Y.Z / X.Y-rcN form used for tags. Empty if no Makefile.
makefile_version() {
    git show "${1}:Makefile" 2>/dev/null \
        | awk -F' = ' '
            $1 == "VERSION"      { v = $2 }
            $1 == "PATCHLEVEL"   { p = $2 }
            $1 == "SUBLEVEL"     { s = $2 }
            $1 == "EXTRAVERSION" { e = $2 }
            END {
                if (v == "") exit
                if (e ~ /^-rc[0-9]+$/) printf "%s.%s%s", v, p, e
                else                   printf "%s.%s.%s", v, p, s
            }'
}

# "devario-6.19.3-2" -> "6.19.3"
tag_version() {
    local t=${1#"${TAG_PREFIX}"-}
    printf '%s' "${t%-*}"
}

# First-parent merge commits of topic branches in <upstream>..<ref>.
# Output: "<merge-hash>\t<branch>" per line, oldest first.
branch_merges() {
    local upstream=$1 ref=$2
    [[ -n $upstream ]] || return 0
    git log --first-parent --merges --reverse --format='%H%x09%s' "${upstream}..${ref}" \
        | sed -nE "s/^([0-9a-f]+)\t[Mm]erge branch '([^']+)'.*/\1\t\2/p"
}

# Commits brought in by merge <hash>, oldest first: "<short-hash>\t<subject>".
merge_commits() {
    git log --no-merges --reverse --format='%h%x09%s' "${1}^1..${1}^2"
}

# Every commit of every topic branch at <ref>: "<branch>\t<subject>".
# Hash-free on purpose, so two releases can be diffed across rebases.
branch_snapshot() {
    local upstream=$1 ref=$2 mhash branch _ subject
    while IFS=$'\t' read -r mhash branch; do
        while IFS=$'\t' read -r _ subject; do
            printf '%s\t%s\n' "$branch" "$subject"
        done < <(merge_commits "$mhash")
    done < <(branch_merges "$upstream" "$ref")
}

# ---------------------------------------------------------------------------
# Changelog rendering (Markdown)
# ---------------------------------------------------------------------------

# Lines of <snapshot> belonging to <branch>, sorted (for comm).
snapshot_subjects() {
    awk -F'\t' -v b="$2" '$1 == b { print $2 }' <<< "$1" | sort
}

render_diff_section() {
    local prev_tag=$1 prev_version=$2 prev_snap=$3 cur_snap=$4
    local prev_branches cur_branches body=''

    prev_branches=$(cut -f1 <<< "$prev_snap" | sort -u | sed '/^$/d')
    cur_branches=$(cut -f1 <<< "$cur_snap"  | sort -u | sed '/^$/d')

    if [[ $prev_version != "$VERSION" ]]; then
        body+="- Rebased onto Linux ${VERSION} (was ${prev_version})"$'\n'
    fi

    local b
    while read -r b; do
        [[ -n $b ]] && body+="- Added branch \`${b}\`"$'\n'
    done < <(comm -13 <(echo "$prev_branches") <(echo "$cur_branches"))

    while read -r b; do
        [[ -n $b ]] && body+="- Removed branch \`${b}\`"$'\n'
    done < <(comm -23 <(echo "$prev_branches") <(echo "$cur_branches"))

    while read -r b; do
        [[ -n $b ]] || continue
        local prev_set cur_set added dropped
        prev_set=$(snapshot_subjects "$prev_snap" "$b")
        cur_set=$(snapshot_subjects "$cur_snap" "$b")
        added=$(comm -13 <(echo "$prev_set") <(echo "$cur_set") | sed '/^$/d')
        dropped=$(comm -23 <(echo "$prev_set") <(echo "$cur_set") | sed '/^$/d')
        [[ -n $added || -n $dropped ]] || continue

        body+="- Updated branch \`${b}\`"$'\n'
        [[ -n $added   ]] && body+=$(sed 's/^/  - `+` /' <<< "$added")$'\n'
        [[ -n $dropped ]] && body+=$(sed 's/^/  - `-` /' <<< "$dropped")$'\n'
    done < <(comm -12 <(echo "$prev_branches") <(echo "$cur_branches"))

    printf '## Changes since %s\n\n' "$prev_tag"
    if [[ -n $body ]]; then
        printf '%s\n' "$body"
    else
        printf 'No changes in applied branches.\n\n'
    fi
}

render_branches_section() {
    local upstream=$1 mhash branch commits count

    printf '## Applied branches\n\n'

    local any=0
    while IFS=$'\t' read -r mhash branch; do
        any=1
        commits=$(merge_commits "$mhash")
        count=$(grep -c . <<< "$commits" || true)
        printf '### %s (%s commit%s)\n\n' "$branch" "$count" "$([[ $count == 1 ]] || echo s)"
        sed -E 's/^([0-9a-f]+)\t(.*)$/- `\1` \2/' <<< "$commits"
        printf '\n'
    done < <(branch_merges "$upstream" HEAD)

    [[ $any == 1 ]] || printf '_No topic branches merged on top of upstream._\n\n'
}

# Full changelog. First line is a plain title (becomes the tag subject),
# everything after the blank line is the Markdown body.
# render_changelog <upstream-hash> [<prev-tag> <prev-upstream-hash>]
render_changelog() {
    local upstream_hash=$1 prev_tag=${2:-} prev_upstream=${3:-}

    printf '%s\n\n' "$TITLE"
    printf 'Based on upstream Linux %s.\n' "$VERSION"
    [[ -n $prev_tag ]] && printf 'Previous release: %s.\n' "$prev_tag"
    printf '\n'

    if [[ -n $prev_tag ]]; then
        render_diff_section "$prev_tag" "$(tag_version "$prev_tag")" \
            "$(branch_snapshot "$prev_upstream" "$prev_tag")" \
            "$(branch_snapshot "$upstream_hash" HEAD)"
    fi

    render_branches_section "$upstream_hash"
}

# ---------------------------------------------------------------------------
# Release steps
# ---------------------------------------------------------------------------

# All base checks for a *new* tag. Every failure is fatal: a signed tag that
# describes the wrong base is worse than no tag at all.
check_release_base() {
    local mk_version newer

    # 1. The tree itself must declare the requested version.
    mk_version=$(makefile_version HEAD)
    [[ -n $mk_version ]] \
        || die "no kernel Makefile at HEAD - is this a kernel checkout?"
    [[ $mk_version == "$VERSION" ]] \
        || die "Makefile at HEAD declares ${mk_version}, not ${VERSION}"

    # 2. The upstream release commit must be in history ...
    UPSTREAM_HASH=$(upstream_commit "$VERSION" HEAD)
    [[ -n $UPSTREAM_HASH ]] \
        || die "upstream commit 'Linux ${VERSION}' not found on the first-parent line of HEAD (an upstream merged in as a side branch does not count as the base)"

    # 3. ... and must be the *current* base: nothing newer from upstream
    #    may sit between it and HEAD.
    newer=$(git log --format=%s "${UPSTREAM_HASH}..HEAD" | grep -E '^Linux [0-9]' | head -n1 || true)
    [[ -z $newer ]] \
        || die "'${newer}' is above 'Linux ${VERSION}' in HEAD history - the tree is not based on ${VERSION}"

    # 4. The previous release of the series must have a usable base too,
    #    otherwise the diff section would be garbage - and it must be the
    #    real previous release, not just the newest tag this clone knows.
    check_tags_synced
    PREV_TAG=$(previous_tag "$MAJOR_MINOR")
    PREV_UPSTREAM_HASH=''
    if [[ -n $PREV_TAG ]]; then
        if grep -qxF "$PREV_TAG" <<< "$UNPUBLISHED_TAGS"; then
            warn "previous release ${PREV_TAG} is not on origin yet - the changelog will refer to an unpublished release"
        fi
        PREV_UPSTREAM_HASH=$(upstream_commit "$(tag_version "$PREV_TAG")" "$PREV_TAG")
        [[ -n $PREV_UPSTREAM_HASH ]] \
            || die "upstream commit 'Linux $(tag_version "$PREV_TAG")' not found on the first-parent line of ${PREV_TAG}"
    fi
}

# Local tags of this channel must match origin: every tag on origin must
# exist locally *and* point at the same object (the previous release and
# the changelog diff are read from local tags). Unpublished local tags are
# allowed - that is the "tag now, --resume later" flow - but are reported.
check_tags_synced() {
    local remote remote_oid ref name local_oid
    UNPUBLISHED_TAGS=''   # global, read by check_release_base; must exist on every return path

    if ! remote=$(git ls-remote --refs --tags origin "refs/tags/${TAG_PREFIX}-[0-9]*" 2>/dev/null); then
        if [[ $DRY_RUN == 1 ]]; then
            warn "origin unreachable - cannot verify that local ${TAG_PREFIX}-* tags match origin"
            return 0
        fi
        die "origin unreachable - cannot verify that local ${TAG_PREFIX}-* tags match origin"
    fi

    while IFS=$'\t' read -r remote_oid ref; do
        [[ -n $ref ]] || continue
        name=${ref#refs/tags/}
        local_oid=$(git rev-parse -q --verify "$ref" 2>/dev/null) \
            || die "tag ${name} exists on origin but not locally - run 'git fetch --tags origin' first"
        [[ $local_oid == "$remote_oid" ]] \
            || die "tag ${name} differs between local (${local_oid:0:12}) and origin (${remote_oid:0:12}) - resolve before releasing"
    done <<< "$remote"

    UNPUBLISHED_TAGS=$(comm -23 \
        <(git tag -l "${TAG_PREFIX}-[0-9]*" | sort) \
        <(sed 's#.*refs/tags/##' <<< "$remote" | sort) | sed '/^$/d')
}

create_tag() {
    # whitespace: the default cleanup (strip) drops "#" lines, i.e. every Markdown
    # heading; verbatim would leave the message without a trailing newline,
    # which breaks signature detection.
    git -c gpg.format=openpgp tag -s --cleanup=whitespace "$TAG" -m "$CHANGELOG"
    ok "created signed tag ${TAG}"
}

# Idempotent: every step is safe to repeat, so --resume can re-run it.
# Artifacts are built in a fresh temporary directory (never next to the
# checkout, never reusing leftovers), and local work happens before anything
# touches the remote.
publish_release() {
    local tarball="${TAG}.tar.gz" workdir tag_key asc_key repo_slug

    repo_slug=$(git remote get-url origin \
        | sed -E 's#^(https://github\.com/|git@github\.com:)##; s#\.git$##')
    [[ $repo_slug == "$EXPECTED_REPO_SLUG" ]] \
        || die "origin is '${repo_slug}', releases go to '${EXPECTED_REPO_SLUG}' only"

    workdir=$(mktemp -d -t "${TAG}.XXXXXX")
    # shellcheck disable=SC2064  # expand now: workdir is local
    trap "rm -rf '${workdir}'" RETURN EXIT

    step "Building source tarball"
    git archive --format=tar.gz --prefix="${TAG}/" -o "${workdir}/${tarball}" "$TAG"
    ok "$tarball"

    step "Signing tarball"
    # Contract: tag and tarball are signed by the same key. The key is taken
    # from the tag's signature, not from user.signingkey, so a key change
    # between tagging and (re)publishing cannot produce a mixed release.
    tag_key=$(tag_signing_key "$TAG")
    [[ -n $tag_key ]] || die "cannot determine the key that signed ${TAG}"
    gpg --batch --list-secret-keys "$tag_key" &>/dev/null \
        || die "secret key ${tag_key} (signer of ${TAG}) is not available - cannot sign the tarball with the tag's key"
    gpg --local-user "$tag_key" \
        --armor --detach-sign -o "${workdir}/${tarball}.asc" "${workdir}/${tarball}"
    asc_key=$(gpg --status-fd 1 --verify "${workdir}/${tarball}.asc" "${workdir}/${tarball}" 2>/dev/null \
        | awk '$1 == "[GNUPG:]" && $2 == "VALIDSIG" { print $3; exit }')
    [[ $asc_key == "$tag_key" ]] \
        || die "tarball signature key (${asc_key:-none}) does not match tag key (${tag_key})"
    ok "${tarball}.asc (key ${tag_key:(-16)})"

    step "Pushing tag"
    local local_oid remote_oid
    local_oid=$(git rev-parse "refs/tags/${TAG}")
    remote_oid=$(git ls-remote --refs origin "refs/tags/${TAG}" | cut -f1)
    if [[ -z $remote_oid ]]; then
        git push origin "refs/tags/${TAG}:refs/tags/${TAG}"
    elif [[ $remote_oid == "$local_oid" ]]; then
        info "already on origin (${remote_oid:0:12})"
    else
        # Never overwrite a published tag. Whoever resolves this decides
        # which object is the real release; the script must not guess.
        die "tag ${TAG} differs between local (${local_oid:0:12}) and origin (${remote_oid:0:12}) - refusing to publish"
    fi

    # Body only - the title line is passed separately.
    sed '1,2d' <<< "$CHANGELOG" > "${workdir}/notes.md"

    if gh release view "$TAG" --repo "$repo_slug" &>/dev/null; then
        step "Updating existing GitHub release (assets only)"
        # Deliberately files-only: title/notes of a published release are
        # left alone. Fix those by hand with 'gh release edit' if needed.
        gh release upload "$TAG" "${workdir}/${tarball}" "${workdir}/${tarball}.asc" \
            --repo "$repo_slug" --clobber
    else
        step "Creating GitHub release${PRERELEASE:+ (pre-release)}"
        gh release create "$TAG" "${workdir}/${tarball}" "${workdir}/${tarball}.asc" \
            --repo "$repo_slug" \
            --title "$TITLE" \
            --notes-file "${workdir}/notes.md" \
            --verify-tag ${PRERELEASE:+--prerelease}
    fi

    local base="https://github.com/${repo_slug}/releases/download/${TAG}"
    ok "release published"
    info "tarball:   ${base}/${tarball}"
    info "signature: ${base}/${tarball}.asc"
}

# --resume: finish publishing an existing signed tag. Works only from the
# tag object - never from HEAD, never recreating or moving the tag.
resume_release() {
    step "Resuming publication of ${TAG}"

    [[ $(git cat-file -t "refs/tags/${TAG}") == tag ]] \
        || die "'${TAG}' is a lightweight tag, not a signed tag object"
    git -c gpg.format=openpgp verify-tag "$TAG" &>/dev/null \
        || die "tag '${TAG}' has no valid GPG signature - refusing to publish it"

    CHANGELOG=$(git tag -l --format='%(contents:subject)%0a%0a%(contents:body)' "$TAG")
    [[ ${CHANGELOG%%$'\n'*} == "$TITLE" ]] \
        || warn "tag message title is '${CHANGELOG%%$'\n'*}', expected '${TITLE}'"

    info "tag:    ${TAG}"
    info "commit: $(git log -1 --format='%h %s' "${TAG}^{commit}")"
    info "key:    $(tag_signing_key "$TAG")"
    info "notes:  taken from the tag message"
    echo
    confirm "Push tag and create/update GitHub release?" || { info "aborted"; return 0; }
    publish_release
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if [[ $RESUME == 1 ]]; then
    resume_release
    exit $?
fi

# Normal release creation follows.
check_release_base

if [[ -n $(git status --porcelain --untracked-files=no) ]]; then
    warn "working tree has uncommitted changes to tracked files - the tag covers HEAD only:"
    git status --porcelain --untracked-files=no | sed 's/^/      /' >&2
fi

step "Release summary"
info "tag:      ${TAG}"
info "branch:   $(git branch --show-current)"
info "commit:   $(git log -1 --format='%h %s')"
info "upstream: $(git log -1 --format='%h %s' "$UPSTREAM_HASH")"
info "previous: ${PREV_TAG:-${C_DIM}(none)${C_RESET}}"
echo

step "Generating changelog"
CHANGELOG=$(render_changelog "$UPSTREAM_HASH" "$PREV_TAG" "$PREV_UPSTREAM_HASH")
echo
printf '%s┌─ changelog ─────────────────────────────────────────────%s\n' "$C_DIM" "$C_RESET"
sed -e "s/^/${C_DIM}│${C_RESET} /" -e 's/ $//' <<< "$CHANGELOG"
printf '%s└─────────────────────────────────────────────────────────%s\n' "$C_DIM" "$C_RESET"
echo

if [[ $DRY_RUN == 1 ]]; then
    info "dry run - nothing was created"
    exit 0
fi

confirm "Create signed tag ${TAG}?" || { info "aborted"; exit 0; }
create_tag
echo

if confirm "Push tag and create GitHub release?"; then
    publish_release
else
    info "tag left local - publish later with: ${SCRIPT_NAME} --resume${CHANNEL:+ --${CHANNEL}} ${VERSION} ${PKGREL}"
fi
