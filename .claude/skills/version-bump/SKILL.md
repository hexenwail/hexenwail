---
name: version-bump
description: Bump and publish a Hexenwail version. Use when asked to bump the version, cut a release revision, write release notes, tag a release, or version bump/tag/push. Edit HW_BASE_VERSION in engine/hexen2/quakedef.h, prepare notes, commit, tag that commit, and push the branch and tag.
---

# Version bump

Use the repository's worktree convention; other sessions may be using the main checkout. Do not create a tag for a version the user has not chosen. If the version is not specified, ask for it rather than guessing. This skill does **not** publish a GitHub release or upload binaries.

1. **Check the starting point.** Fetch `origin master` (avoid `git fetch --all --tags`: this repo has conflicting archived tags), inspect `git status`, the current `HW_BASE_VERSION` in `engine/hexen2/quakedef.h`, the latest release tag, and `docs/release-notes/`. Ensure the release branch/worktree contains the intended commits, and that the proposed tag does not exist locally or remotely. Do not overwrite a tag. Use the project's branch/PR policy for landing the bump; never tag a commit that has not landed if the release is meant to be on `master`.

2. **Prepare notes.** Write `docs/release-notes/<version>.md` from the previous version's notes, retaining applicable Downloads, gamecode, crash-reporting, and requirements sections. Summarize changes in `git log <previous-tag>..HEAD` (not from the previous bump commit), and update badge URLs, the “Changes from” link, commit count, and full-changelog comparison to the correct old and new versions. Read every version reference in the new file before continuing. Do not claim unverified changes or invent artifacts.

3. **Edit the version.** Change `HW_BASE_VERSION` in `engine/hexen2/quakedef.h` to the exact new version. Check that it matches the intended tag and notes filename. Build through Nix (`nix build .#default`) and run `./result/bin/glhexen2 -version` to verify what the binary reports; `HW_GIT_VERSION` can override the header. If verification fails, fix or report it before tagging.

4. **Commit and land.** Stage only the header and notes, review `git diff --cached`, and commit with subject `version(bump) <version>`. Push the worktree branch and land it through a PR if that is the branch policy. A squash merge changes the commit ID: fetch `origin master` after landing, then identify the **landed** bump commit. If releasing directly from a branch, push that branch first.

5. **Tag the landed commit, then push the tag.** In a clean worktree at the intended release commit, create an annotated tag using the committed notes: `git tag -a <version> -F docs/release-notes/<version>.md`. Verify `git rev-parse <version>^{commit}` equals the intended release commit and `git cat-file -t <version>` prints `tag`. Push with `git push origin <version>`; verify the remote branch contains the bump and `git ls-remote --tags origin 'refs/tags/<version>*'` shows the tag and its `^{}` dereference. Report the commit subject, tag, what was pushed, and actual build/version checks. If a push fails, resolve it rather than declaring the release complete.

Do not push unrelated worktree changes, force-update tags, or silently replace the notes/tag with a lightweight tag. If the user only asks to change this skill, do **not** bump or tag a release.
