#!/usr/bin/env python3
"""Build the subtopology index for SubTropica's library.json.

For each topology in the library, computes all edge contractions (subtopologies)
and builds a reverse lookup: subtopology_nickel → [parent topology entries].

This powers the GUI's subtopology matching feature, which shows users which
library topologies contain their drawn graph as a pinched (contracted) subtopology.

The contraction algorithm mirrors SubTropica.wl's Contractions[] function:
iteratively contract one internal edge at a time, delete resulting tadpoles
(self-loops), and filter for 1-vertex-irreducible + non-contact diagrams.

Usage:
    python3 ui/build_subtopology_index.py [--library ui/library.json]

The script reads library.json, computes the subtopology index, and writes
the result back into the same file under the "subtopologies" key.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import defaultdict
from itertools import permutations as itertools_permutations


# ── Nickel parsing ────────────────────────────────────────────────────

# Character → node ID mapping (matching nickel.js convention)
CHAR_TO_NODE: dict[str, int] = {"e": -1}
for _i in range(10):
    CHAR_TO_NODE[str(_i)] = _i
for _i, _c in enumerate("ABCDEF", 10):
    CHAR_TO_NODE[_c] = _i


def parse_nickel(nickel_str: str) -> tuple[list[tuple[int, int]], dict[int, int]]:
    """Parse a bare Nickel string into internal edges and external leg counts.

    Parameters
    ----------
    nickel_str : str
        Bare Nickel index, e.g. "e12|e3|e3|e|".

    Returns
    -------
    edges : list of (u, v) with u < v
        Internal edges (may contain duplicates for multi-edges).
    ext_legs : dict
        Mapping vertex → number of external legs at that vertex.
    """
    parts = nickel_str.split("|")
    # Remove trailing empty parts (Nickel strings end with |)
    while parts and parts[-1] == "":
        parts.pop()

    edges: list[tuple[int, int]] = []
    ext_legs: dict[int, int] = defaultdict(int)

    for v, part in enumerate(parts):
        for ch in part:
            node = CHAR_TO_NODE.get(ch)
            if node is None:
                raise ValueError(
                    f"Unknown character '{ch}' in Nickel string '{nickel_str}'"
                )
            if node == -1:  # external leg
                ext_legs[v] += 1
            elif node > v:  # upper-triangular: only count edge once
                edges.append((v, node))

    return edges, dict(ext_legs)


# ── Edge contraction ──────────────────────────────────────────────────


def contract_edge(
    tagged_edges: list[tuple[int, tuple[int, int]]],
    ext_legs: dict[int, int],
    local_idx: int,
) -> tuple[list[tuple[int, tuple[int, int]]], dict[int, int], set[int]]:
    """Contract the edge at local_idx, merging the higher vertex into the lower.

    Removes resulting self-loops (tadpoles).

    Parameters
    ----------
    tagged_edges : list of (original_idx, (u, v))
        Current edges tagged with their original parent-topology edge index.
    ext_legs : dict
        Current external-leg counts per vertex.
    local_idx : int
        Index into tagged_edges of the edge to contract.

    Returns
    -------
    new_tagged : list of (original_idx, (u, v))
        Surviving edges after contraction and tadpole removal.
    new_ext : dict
        Updated external-leg counts.
    removed : set of int
        Original edge indices that were removed (contracted + tadpoles).
    """
    orig_idx, (eu, ev) = tagged_edges[local_idx]
    # Merge higher vertex into lower
    lo, hi = (eu, ev) if eu < ev else (ev, eu)

    removed: set[int] = {orig_idx}
    new_tagged: list[tuple[int, tuple[int, int]]] = []

    for j, (oj, (a, b)) in enumerate(tagged_edges):
        if j == local_idx:
            continue
        # Redirect: replace hi → lo
        a2 = lo if a == hi else a
        b2 = lo if b == hi else b
        # Canonical order
        if a2 > b2:
            a2, b2 = b2, a2
        # Self-loop (tadpole) → remove
        if a2 == b2:
            removed.add(oj)
            continue
        new_tagged.append((oj, (a2, b2)))

    # Merge external legs from hi into lo
    new_ext = dict(ext_legs)
    if hi in new_ext:
        new_ext[lo] = new_ext.get(lo, 0) + new_ext.pop(hi)

    return new_tagged, new_ext, removed


# ── Graph predicates ──────────────────────────────────────────────────


def _vertices_from_edges(edges: list[tuple[int, int]]) -> set[int]:
    """Collect all vertices appearing in an edge list."""
    verts: set[int] = set()
    for u, v in edges:
        verts.add(u)
        verts.add(v)
    return verts


def _is_connected(
    edges: list[tuple[int, int]], vertices: set[int] | None = None
) -> bool:
    """Check if the graph is connected."""
    if not edges:
        return (vertices is None) or len(vertices) <= 1
    if vertices is None:
        vertices = _vertices_from_edges(edges)
    if not vertices:
        return True

    adj: dict[int, set[int]] = defaultdict(set)
    for u, v in edges:
        adj[u].add(v)
        adj[v].add(u)

    start = next(iter(vertices))
    visited = {start}
    stack = [start]
    while stack:
        node = stack.pop()
        for nbr in adj[node]:
            if nbr not in visited:
                visited.add(nbr)
                stack.append(nbr)

    return visited >= vertices


def is_one_vertex_irreducible(edges: list[tuple[int, int]]) -> bool:
    """Check if the internal-edge graph is 1-vertex-irreducible (biconnected).

    A graph is 1VI if removing any single internal vertex leaves it connected.
    Matches the semantics of OneVertexIrreducibleQ in SubTropica.wl.
    """
    if not edges:
        return False  # contact diagram

    vertices = _vertices_from_edges(edges)

    if len(vertices) <= 2:
        # Two-vertex multigraph: 1VI iff ≥ 2 edges
        return len(edges) >= 2

    # For each vertex, remove it and check connectivity of the rest
    for rm in vertices:
        remaining_verts = vertices - {rm}
        remaining_edges = [(u, v) for u, v in edges if u != rm and v != rm]
        if not _is_connected(remaining_edges, remaining_verts):
            return False

    return True


# ── Canonical Nickel computation (Expander algorithm) ─────────────────
#
# Ported from nickel.js (which ports the Python 2 GraphState library by
# Batkovich, Kirienko, Kompaniets & Novikov, arXiv:1409.8227).
# Uses branch-and-bound expansion, efficient for all graph sizes.

LEG = -1  # sentinel for external legs


def _adjacent_nodes(node: int, edges: list[tuple[int, int]]) -> list[int]:
    """Return all neighbors of `node` in the edge list (with multiplicity)."""
    nbrs: list[int] = []
    for a, b in edges:
        if a == node:
            nbrs.append(b)
        elif b == node:
            nbrs.append(a)
    return nbrs


def _compare_lists(a: list[int], b: list[int]) -> int:
    """Lexicographic comparison of two integer lists."""
    for x, y in zip(a, b):
        if x < y:
            return -1
        if x > y:
            return 1
    return (len(a) > len(b)) - (len(a) < len(b))


def _compare_nickel_lists(a: list[list[int]], b: list[list[int]]) -> int:
    """Lexicographic comparison of nested Nickel lists."""
    for sa, sb in zip(a, b):
        c = _compare_lists(sa, sb)
        if c != 0:
            return c
    return (len(a) > len(b)) - (len(a) < len(b))


def _map_nodes(mapping: dict[int, int], nodes: list[int]) -> list[int]:
    """Apply node relabeling and sort."""
    return sorted(mapping.get(n, n) for n in nodes)


def _map_edges(
    mapping: dict[int, int], edges: list[tuple[int, int]]
) -> list[tuple[int, int]]:
    """Apply node relabeling to edge list."""
    result: list[tuple[int, int]] = []
    for a, b in edges:
        a2 = mapping.get(a, a)
        b2 = mapping.get(b, b)
        result.append((min(a2, b2), max(a2, b2)))
    return result


class _Expander:
    """State in the branch-and-bound Nickel canonicalization."""

    __slots__ = ("edges", "nickel_list", "node_map", "curr_node", "free_node")

    def __init__(
        self,
        edges: list[tuple[int, int]],
        nickel_list: list[list[int]],
        node_map: dict[int, int],
        curr_node: int,
        free_node: int,
    ):
        self.edges = edges
        self.nickel_list = nickel_list
        self.node_map = node_map
        self.curr_node = curr_node
        self.free_node = free_node

    def compare_to(self, other: "_Expander") -> int:
        min_len = min(len(self.nickel_list), len(other.nickel_list))
        return _compare_nickel_lists(
            self.nickel_list[:min_len], other.nickel_list[:min_len]
        )

    def expand(self) -> list["_Expander"]:
        """Generate all expansions of the current node."""
        nodes = _adjacent_nodes(self.curr_node, self.edges)
        edge_rest = [
            e
            for e in self.edges
            if e[0] != self.curr_node and e[1] != self.curr_node
        ]

        # New nodes: adjacent but not yet assigned a canonical label
        new_nodes = sorted(n for n in set(nodes) if n >= self.free_node and n != LEG)
        free_labels = list(range(self.free_node, self.free_node + len(new_nodes)))

        results: list[_Expander] = []
        for perm in itertools_permutations(free_labels):
            local_map = {new_nodes[i]: perm[i] for i in range(len(new_nodes))}
            expanded_nodes = _map_nodes(local_map, nodes)
            mapped_edges = _map_edges(local_map, edge_rest)
            combined_map = dict(self.node_map)
            combined_map.update(local_map)

            results.append(
                _Expander(
                    mapped_edges,
                    self.nickel_list + [expanded_nodes],
                    combined_map,
                    self.curr_node + 1,
                    self.free_node + len(new_nodes),
                )
            )
        return results


def _nickel_list_to_string(nickel_list: list[list[int]]) -> str:
    """Convert nested Nickel list to string representation."""
    NODE_TO_CHAR = {LEG: "e", 10: "A", 11: "B", 12: "C", 13: "D", 14: "E", 15: "F"}
    parts: list[str] = []
    for nn in nickel_list:
        chars: list[str] = []
        for n in nn:
            if n in NODE_TO_CHAR:
                chars.append(NODE_TO_CHAR[n])
            else:
                chars.append(str(n))
        parts.append("".join(chars))
    return "|".join(parts) + "|"


def canonical_nickel(
    edges: list[tuple[int, int]], ext_legs: dict[int, int]
) -> str:
    """Compute the canonical bare Nickel index for a graph.

    Uses the Expander branch-and-bound algorithm (ported from nickel.js).
    Correct and efficient for all graph sizes.

    Parameters
    ----------
    edges : list of (u, v)
        Internal edges.
    ext_legs : dict
        Vertex → number of external legs.

    Returns
    -------
    Canonical bare Nickel string (e.g. "e12|e3|e3|e|").
    """
    # Build full edge list including external legs as LEG (-1) nodes
    all_edges: list[tuple[int, int]] = []
    for u, v in edges:
        all_edges.append((min(u, v), max(u, v)))
    for v, count in ext_legs.items():
        for _ in range(count):
            all_edges.append((LEG, v))

    if not all_edges:
        return ""

    # Count internal nodes
    internal_nodes: set[int] = set()
    for a, b in all_edges:
        if a >= 0:
            internal_nodes.add(a)
        if b >= 0:
            internal_nodes.add(b)
    num_internal = len(internal_nodes)
    if num_internal == 0:
        return ""

    # Shift original node IDs to high values to make room for canonical labels
    offset = max(100, num_internal + 10)

    def shift(n: int) -> int:
        return n + offset if n >= 0 else LEG

    shifted_edges = [(shift(a), shift(b)) for a, b in all_edges]

    # Determine boundary nodes (those adjacent to external legs, or all if no legs)
    all_shifted: set[int] = set()
    for a, b in shifted_edges:
        if a != LEG:
            all_shifted.add(a)
        if b != LEG:
            all_shifted.add(b)

    has_legs = any(a == LEG or b == LEG for a, b in shifted_edges)
    if has_legs:
        boundary = set(_adjacent_nodes(LEG, shifted_edges))
    else:
        boundary = all_shifted

    # Initialize: one Expander per possible starting node
    states: list[_Expander] = []
    for node in boundary:
        node_map = {node: 0}
        states.append(
            _Expander(
                _map_edges(node_map, shifted_edges),
                [],
                node_map,
                0,
                1,
            )
        )

    # Expand one node at a time, pruning non-minimal states
    for _ in range(num_internal):
        next_states: list[_Expander] = []
        for s in states:
            next_states.extend(s.expand())

        if not next_states:
            break

        # Keep only lexicographically minimal states
        minimum = next_states[0]
        for s in next_states[1:]:
            if s.compare_to(minimum) < 0:
                minimum = s
        states = [s for s in next_states if s.compare_to(minimum) == 0]

    return _nickel_list_to_string(states[0].nickel_list)


# ── Iterative contraction (subtopology enumeration) ───────────────────


def all_subtopologies(
    nickel_str: str,
) -> list[tuple[str, list[int]]]:
    """Compute all subtopologies of a topology by iterative edge contraction.

    Mirrors SubTropica.wl's Contractions[] function: at each level, contract
    one internal edge, delete tadpoles, filter for 1VI + non-contact, dedup
    by canonical Nickel.

    Parameters
    ----------
    nickel_str : str
        Bare Nickel index of the parent topology.

    Returns
    -------
    List of (subtopo_nickel, contracted_edges) where:
        subtopo_nickel : str — canonical bare Nickel of the subtopology
        contracted_edges : list of int — original edge indices that were removed
    Does not include the parent itself (0 contractions).
    """
    parent_edges, parent_ext = parse_nickel(nickel_str)
    n_edges = len(parent_edges)

    if n_edges <= 1:
        return []

    # Tag each edge with its original index
    tagged = [(i, e) for i, e in enumerate(parent_edges)]

    results: list[tuple[str, list[int]]] = []
    seen_nickels: set[str] = set()

    # BFS by contraction depth
    # State: (tagged_edges, ext_legs, frozenset of removed original indices)
    current_level: list[
        tuple[list[tuple[int, tuple[int, int]]], dict[int, int], frozenset[int]]
    ] = [(tagged, parent_ext, frozenset())]

    for depth in range(1, n_edges):
        next_level_by_nickel: dict[
            str,
            tuple[list[tuple[int, tuple[int, int]]], dict[int, int], frozenset[int]],
        ] = {}

        for cur_tagged, cur_ext, cur_removed in current_level:
            for local_idx in range(len(cur_tagged)):
                new_tagged, new_ext, step_removed = contract_edge(
                    cur_tagged, cur_ext, local_idx
                )

                # Non-contact check: must have at least one internal edge
                if not new_tagged:
                    continue

                raw_edges = [e for _, e in new_tagged]

                # 1VI check
                if not is_one_vertex_irreducible(raw_edges):
                    continue

                # Canonicalize
                try:
                    cn = canonical_nickel(raw_edges, new_ext)
                except Exception:
                    continue

                all_removed = cur_removed | step_removed

                # Record if this is a new subtopology
                if cn not in seen_nickels:
                    seen_nickels.add(cn)
                    results.append((cn, sorted(all_removed)))

                # Keep one representative per Nickel for next level
                if cn not in next_level_by_nickel:
                    next_level_by_nickel[cn] = (new_tagged, new_ext, all_removed)

        current_level = list(next_level_by_nickel.values())
        if not current_level:
            break

    return results


# ── Index builder ─────────────────────────────────────────────────────


def build_subtopology_index(
    topologies: dict,
    verbose: bool = True,
) -> dict[str, list[dict]]:
    """Build the subtopology reverse-lookup index.

    Parameters
    ----------
    topologies : dict
        The "topologies" dict from library.json, keyed by Nickel string.
    verbose : bool
        If True, print per-topology progress.

    Returns
    -------
    Dict mapping subtopology bare Nickel → list of
    {"p": parent_library_key, "e": [contracted_edge_indices]}.
    """
    index: dict[str, list[dict]] = defaultdict(list)
    total = len(topologies)

    for i, (topo_key, topo) in enumerate(topologies.items()):
        props = topo.get("props", 0)
        if props <= 1:
            continue  # no meaningful contractions

        name = topo.get("name", topo_key)
        if verbose:
            print(
                f"  [{i + 1}/{total}] {name} ({props} props) ...",
                end="",
                flush=True,
            )
        t0 = time.time()

        try:
            subs = all_subtopologies(topo_key)
        except Exception as exc:
            if verbose:
                print(f" ERROR: {exc}")
            continue

        for sub_nickel, removed_edges in subs:
            # Don't add self-references (subtopology = parent)
            if sub_nickel == topo_key:
                continue
            index[sub_nickel].append({"p": topo_key, "e": removed_edges})

        dt = time.time() - t0
        if verbose:
            print(f" {len(subs)} subtopologies ({dt:.1f}s)")

    return dict(index)


# ── Main ──────────────────────────────────────────────────────────────


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build subtopology index for SubTropica library.json."
    )
    parser.add_argument(
        "--library",
        default="ui/library.json",
        help="Path to library.json (default: ui/library.json)",
    )
    parser.add_argument(
        "--quiet", "-q",
        action="store_true",
        help="Suppress per-topology progress output",
    )
    args = parser.parse_args()

    if not args.quiet:
        print(f"Reading {args.library} ...")
    with open(args.library) as f:
        library = json.load(f)

    topologies = library.get("topologies", {})
    if not args.quiet:
        print(f"Found {len(topologies)} topologies.\n")
        print("Building subtopology index:")

    t0 = time.time()
    index = build_subtopology_index(topologies, verbose=not args.quiet)
    dt = time.time() - t0

    n_entries = sum(len(v) for v in index.values())
    if not args.quiet:
        print(f"\nDone: {len(index)} unique subtopology keys, {n_entries} total entries ({dt:.1f}s)")

    # Write back to library.json
    library["subtopologies"] = index
    with open(args.library, "w") as f:
        # Match the formatting Mathematica's ExportString[..., "RawJSON"]
        # produces on the kernel-side build step (tab-indented, compact
        # key:value separator) so re-running the indexer doesn't churn
        # every line of the committed file.
        json.dump(library, f, indent='\t', separators=(',', ':'))

    print(f"Updated {args.library}")


if __name__ == "__main__":
    main()
