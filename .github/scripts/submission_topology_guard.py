#!/usr/bin/env python3
"""Submission topology guard for the public-library submit workflow.

Called by .github/workflows/submit.yml after the mass-consistency check
and BEFORE any directory is created. Exit 0 means the submission may be
written; exit 1 means reject, with the reason on stdout.

Two checks, both born from the 2026-06-06 submission incident
(PRs #36/#39 on SubTropica/SubTropica):

  1. key-vs-graph: the submitted bare Nickel key must decode to a graph
     isomorphic (external legs included) to the payload's edges/nodes
     graph. A key that does not describe the submitted graph would file
     the result under a wrong address.

  2. duplicate-topology: a submission whose topology directory does NOT
     yet exist must not be isomorphic to any existing library topology.
     The workflow writes the submitter's labeling verbatim, so a
     non-canonical key silently creates a duplicate directory for an
     existing topology (PR #39 created e12|e3|33|| although the library
     already holds the same graph as canonical 112|3|e3|e|, a directory
     the v1.0.140 canonical-Nickel dedup had deliberately removed).

Self-contained by necessity: the public repository ships no scripts/
directory, so the decoder and isomorphism test are COPIED from the
dev-side scripts/nickel_parse_check.py (the authoritative multi-digit
Nickel decoder). The dev-side pytest
scripts/tests/test_submission_topology_guard.py keeps the two copies
behaviorally in sync over the full library corpus; edit BOTH files or
the sync test fails.

External legs are compared by decorating each leg as an edge to a fresh
phantom vertex, then running the plain multigraph isomorphism test.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Optional


# --------------------------------------------------------------------------
# Decoder (copied from scripts/nickel_parse_check.py; keep in sync)
# --------------------------------------------------------------------------
def _tokenize_stanza(s: str, k: int, nv: int) -> Optional[list[int]]:
    """Backtracking tokenizer for the numeric tail of stanza k.

    Returns a nondecreasing list of labels in [k, nv-1] that consumes all
    of ``s``, or ``None`` if no such tokenization exists. Resolves "810"
    as [8, 10] (not [8, 1, 0]) because 1 < 8 violates nondecreasing and
    0 < k.
    """
    def rec(i: int, lo: int) -> Optional[list[int]]:
        if i == len(s):
            return []
        for ln in (1, 2):  # decimal labels are 1 or 2 digits (nv <= 99)
            if i + ln > len(s):
                continue
            cand = s[i:i + ln]
            if len(cand) > 1 and cand[0] == "0":
                continue  # no leading-zero multi-digit labels
            val = int(cand)
            if not (lo <= val <= nv - 1):
                continue
            rest = rec(i + ln, val)
            if rest is not None:
                return [val] + rest
        return None

    return rec(0, k)


def parse_bare_nickel_with_legs(
        bare: str) -> tuple[list[tuple[int, int]], dict[int, int], int]:
    """Decode a BARE Nickel string into (edges, legs_per_vertex, nv).

    Differs from nickel_parse_check.parse_bare_nickel in two ways: nv is
    inferred from the stanza count (a directory name carries no separate
    vertex count), and the per-stanza external-leg counts ('e' prefixes)
    are returned instead of discarded (the guard compares leg placement).
    Raises ValueError on an untokenizable stanza.
    """
    parts = bare.split("|")
    if parts and parts[-1] == "":
        parts = parts[:-1]  # drop the trailing "|"
    nv = len(parts)
    edges: list[tuple[int, int]] = []
    legs: dict[int, int] = {}
    for k, stanza in enumerate(parts):
        s = stanza
        nlegs = 0
        while s.startswith("e"):
            nlegs += 1
            s = s[1:]
        if nlegs:
            legs[k] = nlegs
        labels = _tokenize_stanza(s, k, nv)
        if labels is None:
            raise ValueError(
                f"stanza {k}={stanza!r} untokenizable under [k,nv-1] "
                f"nondecreasing constraints (nv={nv})")
        edges.extend((k, v) for v in labels)
    return edges, legs, nv


# --------------------------------------------------------------------------
# Isomorphism (copied from scripts/nickel_parse_check.py; keep in sync)
# --------------------------------------------------------------------------
def _adj(edges: list[tuple[int, int]], nv: int) -> dict[int, list[int]]:
    a: dict[int, list[int]] = {v: [] for v in range(nv)}
    for x, y in edges:
        a[x].append(y)
        a[y].append(x)
    return a


def _wl_colors(adj: dict[int, list[int]], rounds: int = 12) -> dict[int, int]:
    """1-WL color refinement (init by degree), compressed to ints."""
    col = {v: len(adj[v]) for v in adj}
    for _ in range(rounds):
        sig = {v: (col[v], tuple(sorted(col[u] for u in adj[v]))) for v in adj}
        cmap = {s: i for i, s in enumerate(sorted(set(sig.values()), key=str))}
        new = {v: cmap[sig[v]] for v in sig}
        if new == col:
            break
        col = new
    return col


def graphs_isomorphic(e1: list[tuple[int, int]],
                      e2: list[tuple[int, int]], nv: int) -> bool:
    """True iff the two 0-indexed edge lists (nv vertices each) are
    isomorphic as undirected multigraphs. 1-WL prune + VF2-lite backtrack."""
    if len(e1) != len(e2):
        return False
    a1, a2 = _adj(e1, nv), _adj(e2, nv)
    c1, c2 = _wl_colors(a1), _wl_colors(a2)
    if Counter(c1.values()) != Counter(c2.values()):
        return False
    cand_by_col: dict[int, list[int]] = defaultdict(list)
    for v, c in c2.items():
        cand_by_col[c].append(v)
    mult1: dict[tuple[int, int], int] = Counter(tuple(sorted(e)) for e in e1)
    mult2: dict[tuple[int, int], int] = Counter(tuple(sorted(e)) for e in e2)
    order = sorted(range(nv), key=lambda v: len(cand_by_col[c1[v]]))
    mapping: dict[int, int] = {}
    used: set[int] = set()

    def consistent(v: int, w: int) -> bool:
        for u in a1[v]:
            if u in mapping:
                if mult1[tuple(sorted((v, u)))] != \
                        mult2[tuple(sorted((w, mapping[u])))]:
                    return False
        return True

    def bt(i: int) -> bool:
        if i == len(order):
            return True
        v = order[i]
        for w in cand_by_col[c1[v]]:
            if w in used or not consistent(v, w):
                continue
            mapping[v] = w
            used.add(w)
            if bt(i + 1):
                return True
            del mapping[v]
            used.discard(w)
        return False

    return bt(0)


# --------------------------------------------------------------------------
# Guard-specific machinery
# --------------------------------------------------------------------------
def decorate_legs(edges: list[tuple[int, int]], legs: dict[int, int],
                  nv: int) -> tuple[list[tuple[int, int]], int]:
    """Append one phantom degree-1 vertex per external leg.

    Leg placement then participates in the plain multigraph isomorphism
    test: two graphs with isomorphic internal skeletons but different
    leg attachments are NOT identified.
    """
    dec = list(edges)
    nxt = nv
    for v in sorted(legs):
        for _ in range(legs[v]):
            dec.append((v, nxt))
            nxt += 1
    return dec, nxt


def parse_mma_edges(edges_str: str) -> list[tuple[int, int]]:
    """Internal edges from the payload's Mma string.

    Format "{{{1, 2}, m}, {{1, 2}, 0}, ...}": every {int, int} pair is an
    edge endpoint pair (mass slots are symbols or 0, never bare integer
    pairs, so the regex cannot over-match). Same extraction the workflow
    already uses for vertex counting.
    """
    return [(int(m.group(1)), int(m.group(2)))
            for m in re.finditer(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", edges_str)]


def parse_mma_leg_vertices(nodes_str: str) -> list[int]:
    """External-leg vertices from the payload's Mma nodes string.

    Format "{{1, M}, {3, M[1]}, ...}": the first element of each top-level
    pair is the vertex carrying the leg (library convention stores one
    combined leg per vertex, but repeats are tolerated here).
    """
    return [int(m.group(1))
            for m in re.finditer(r"\{\s*(\d+)\s*,", nodes_str or "")]


def payload_graph(edges_str: str, nodes_str: str
                  ) -> tuple[list[tuple[int, int]], dict[int, int], int]:
    """Normalize the payload graph to 0-indexed (edges, legs, nv)."""
    raw_edges = parse_mma_edges(edges_str)
    raw_legs = parse_mma_leg_vertices(nodes_str)
    verts = sorted({v for e in raw_edges for v in e} | set(raw_legs))
    relab = {v: i for i, v in enumerate(verts)}
    edges = [(relab[a], relab[b]) for a, b in raw_edges]
    legs: dict[int, int] = {}
    for v in raw_legs:
        legs[relab[v]] = legs.get(relab[v], 0) + 1
    return edges, legs, len(verts)


def dir_to_bare(dirname: str) -> str:
    """Topology directory name back to its bare Nickel string.

    The workflow's sanitize() maps "|" to "_" (topology dirs carry no
    ":"), so the inverse is faithful for topology-level names.
    """
    return dirname.replace("_", "|")


def signature(edges: list[tuple[int, int]], legs: dict[int, int],
              nv: int) -> tuple:
    """Cheap pre-filter key: iso-invariant, collision-prone by design."""
    deg = Counter()
    for a, b in edges:
        deg[a] += 1
        deg[b] += 1
    degseq = tuple(sorted((deg.get(v, 0), legs.get(v, 0))
                          for v in range(nv)))
    return (nv, len(edges), sum(legs.values()), degseq)


def scan_library(library: Path):
    """Yield (dirname, edges, legs, nv) for every decodable topology dir.

    Undecodable names are skipped with a warning: the guard must never
    block a submission because some unrelated legacy directory predates
    the naming conventions.
    """
    for d in sorted(library.iterdir()):
        if not d.is_dir():
            continue
        try:
            edges, legs, nv = parse_bare_nickel_with_legs(dir_to_bare(d.name))
        except (ValueError, IndexError) as e:
            print(f"guard: WARNING: skipping undecodable dir {d.name!r}: {e}")
            continue
        yield d.name, edges, legs, nv


def check_submission(bare: str, edges_str: str, nodes_str: str,
                     library: Path) -> Optional[str]:
    """Run both checks. Returns None when OK, else the rejection reason."""
    p_edges, p_legs, p_nv = payload_graph(edges_str, nodes_str)
    if not p_edges:
        # No parseable graph in the payload: nothing to validate against.
        # The mass-consistency check and schema validation still apply.
        print("guard: payload carries no parseable edges; "
              "topology checks skipped")
        return None

    # Check 1: the submitted key must describe the submitted graph.
    try:
        k_edges, k_legs, k_nv = parse_bare_nickel_with_legs(bare)
    except (ValueError, IndexError) as e:
        return (f"bare Nickel key {bare!r} is not decodable: {e}. "
                "The key cannot be verified against the submitted graph; "
                "rejecting (no entry written).")
    dec_p, nv_p = decorate_legs(p_edges, p_legs, p_nv)
    dec_k, nv_k = decorate_legs(k_edges, k_legs, k_nv)
    if nv_p != nv_k or not graphs_isomorphic(dec_p, dec_k, nv_p):
        return (f"bare Nickel key {bare!r} decodes to a graph that is NOT "
                "isomorphic (legs included) to the submitted edges/nodes "
                "graph. The key does not describe the submitted integral; "
                "rejecting (no entry written).")

    # Check 2: a NEW topology directory must not duplicate an existing one.
    sanitized = bare.replace("|", "_").replace(":", "_")
    if (library / sanitized).is_dir():
        return None  # existing directory: canonical home already decided
    sig = signature(p_edges, p_legs, p_nv)
    for name, e_edges, e_legs, e_nv in scan_library(library):
        if signature(e_edges, e_legs, e_nv) != sig:
            continue
        dec_e, nv_e = decorate_legs(e_edges, e_legs, e_nv)
        if nv_e == nv_p and graphs_isomorphic(dec_p, dec_e, nv_p):
            return (f"submission would create NEW topology dir "
                    f"{sanitized!r}, but the graph is isomorphic to the "
                    f"EXISTING library topology {name!r}. The submitted "
                    "key is a non-canonical relabeling; re-key the "
                    "submission to the existing canonical index "
                    "(rejecting; no entry written).")
    return None


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bare", required=True,
                    help="bare Nickel key (before ':') of the submission")
    ap.add_argument("--edges", required=True,
                    help="payload edges as the Mma list string")
    ap.add_argument("--nodes", default="",
                    help="payload nodes as the Mma list string")
    ap.add_argument("--library", default="library-bundled",
                    help="library root to scan for duplicates")
    args = ap.parse_args(argv)
    reason = check_submission(args.bare, args.edges, args.nodes,
                              Path(args.library))
    if reason is not None:
        print("TOPOLOGY-GUARD FAILURE: " + reason)
        return 1
    print("guard: topology checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
