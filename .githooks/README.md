# Repo-versioned git hooks

The hooks in this directory are versioned with the repository so they propagate
to all clones. Git ignores them by default; opt in once per clone:

    git config core.hooksPath .githooks

## pre-commit

Regenerates `ui/library.json` whenever a commit modifies any
`library-bundled/*.json` (other than `manifest.json`). Runs the same two-step
pipeline as `scripts/publish-release.sh`:

1. `scripts/_build_library_json.py` — rolls up entry/topology JSON
2. `ui/build_subtopology_index.py` — appends subtopologies, tab-indents

Without it, hand edits, bot-merged submits, and cross-repo entry promotions
leave the rollup stale until the next release cut. (See the v1.1.4 cycle for an
example: a public-side submit landed on dev as an entry-only diff with the
rollup unrefreshed.)

Bypass for an individual commit:

    SKIP_LIBRARY_REBUILD=1 git commit ...

Requires `python3` on `$PATH`. Hook is a no-op if the staged change is purely
outside `library-bundled/`.
