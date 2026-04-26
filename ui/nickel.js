/**
 * nickel.js — Canonical Nickel index computation for Feynman diagrams.
 *
 * Ported from the Python 2 GraphState library by Batkovich, Kirienko,
 * Kompaniets & Novikov (arXiv:1409.8227). Original code:
 * https://github.com/batya239/graph-state
 *
 * Nickel notation encodes a graph as a string built from the upper-triangular
 * adjacency lists of the canonically labeled vertices. External legs are
 * denoted 'e'. The canonical labeling is the one that yields the
 * lexicographically smallest such string.
 */

const LEG = -1;
const SEP_ID = -600;

// Vertex label encoding: 0..9 use digits, 10..35 use uppercase letters A..Z.
// This matches the Loopedia convention and nickel_from_edges.py / SubTropica.wl.
// Maximum supported graph: 36 vertices.
const NODE_TO_CHAR = new Map([
  [SEP_ID, '|'], [LEG, 'e'],
]);
for (let i = 10; i < 36; i++) {
  NODE_TO_CHAR.set(i, String.fromCharCode('A'.charCodeAt(0) + (i - 10)));
}

const CHAR_TO_NODE = new Map(
  Array.from(NODE_TO_CHAR.entries()).map(([k, v]) => [v, k])
);

// ─── Utility functions ───────────────────────────────────────────────

function flatten(arr) {
  const result = [];
  for (const sub of arr) {
    for (const item of sub) {
      result.push(item);
    }
  }
  return result;
}

function sortedEdge(e) {
  return e[0] <= e[1] ? [e[0], e[1]] : [e[1], e[0]];
}

function adjacentNodes(node, edges) {
  const nodes = [];
  for (const e of edges) {
    if (e[0] === node) nodes.push(e[1]);
    else if (e[1] === node) nodes.push(e[0]);
  }
  return nodes;
}

function isConnected(edges) {
  if (edges.length === 0) return false;
  const visited = new Set(edges[0]);
  let oldSize = 0;
  while (oldSize < visited.size) {
    oldSize = visited.size;
    for (const edge of edges) {
      if (visited.has(edge[0]) || visited.has(edge[1])) {
        visited.add(edge[0]);
        visited.add(edge[1]);
      }
    }
  }
  const allNodes = new Set(flatten(edges));
  return visited.size === allNodes.size;
}

/** All permutations of an array. */
function* permutations(arr) {
  if (arr.length <= 1) {
    yield arr.slice();
    return;
  }
  for (let i = 0; i < arr.length; i++) {
    const rest = arr.slice(0, i).concat(arr.slice(i + 1));
    for (const perm of permutations(rest)) {
      perm.unshift(arr[i]);
      yield perm;
    }
  }
}

function mapNodes1(dic, listOfNodes) {
  return listOfNodes.map(n => (dic.has(n) ? dic.get(n) : n)).sort((a, b) => a - b);
}

function mapNodes2(dic, listOfLists) {
  return listOfLists.map(x => mapNodes1(dic, x));
}

/** Lexicographic comparison of two arrays of numbers. */
function compareLists(a, b) {
  const len = Math.min(a.length, b.length);
  for (let i = 0; i < len; i++) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  return a.length - b.length;
}

/** Lexicographic comparison of nested lists (Nickel lists). */
function compareNickelLists(a, b) {
  const len = Math.min(a.length, b.length);
  for (let i = 0; i < len; i++) {
    const c = compareLists(a[i], b[i]);
    if (c !== 0) return c;
  }
  return a.length - b.length;
}

// ─── Nickel class ────────────────────────────────────────────────────

/**
 * Convert between graph representations: edge list, Nickel list, Nickel string.
 *
 * Usage:
 *   const n = Nickel.fromEdges([[-1, 0], [0, 1], [1, -1]]);
 *   n.nickel   // [[-1, 1], [-1]]
 *   n.string   // 'e1|e|'
 */
class Nickel {
  constructor(nickelList) {
    this._nickel = nickelList;
    this._edges = null;
    this._string = null;
    this._adjacent = null;
  }

  static fromEdges(edges) {
    return new Nickel(Nickel.nickelFromEdges(edges));
  }

  static fromNickel(nickelList) {
    return new Nickel(nickelList.map(nn => nn.slice().sort((a, b) => a - b)));
  }

  static fromString(str) {
    const n = new Nickel(Nickel.nickelFromString(str));
    n._string = str;
    return n;
  }

  get nickel() { return this._nickel; }

  get edges() {
    if (this._edges === null) {
      this._edges = Nickel.edgesFromNickel(this._nickel);
    }
    return this._edges;
  }

  get string() {
    if (this._string === null) {
      this._string = Nickel.stringFromNickel(this._nickel);
    }
    return this._string;
  }

  get adjacent() {
    if (this._adjacent === null) {
      this._adjacent = Nickel.nickelToAdjacent(this._nickel);
    }
    return this._adjacent;
  }

  static nickelFromEdges(edges) {
    const allNodes = flatten(edges);
    const nodeLimit = Math.max(...allNodes) + 1;
    const nickel = [];
    for (let i = 0; i < nodeLimit; i++) nickel.push([]);
    for (const e of edges) {
      const sorted = e.slice().sort((a, b) => {
        const ka = a >= 0 ? a : nodeLimit;
        const kb = b >= 0 ? b : nodeLimit;
        return ka - kb;
      });
      nickel[sorted[0]].push(sorted[1]);
    }
    for (const nn of nickel) nn.sort((a, b) => a - b);
    return nickel;
  }

  static edgesFromNickel(nickel) {
    const edges = [];
    for (let n = 0; n < nickel.length; n++) {
      for (const m of nickel[n]) {
        edges.push(sortedEdge([n, m]));
      }
    }
    return edges;
  }

  static stringFromNickel(nickel) {
    const chars = [];
    for (const nn of nickel) {
      for (const n of nn) {
        chars.push(NODE_TO_CHAR.has(n) ? NODE_TO_CHAR.get(n) : String(n));
      }
      chars.push('|');
    }
    return chars.join('');
  }

  static nickelFromString(str) {
    // Two encodings appear in the wild:
    //   - Loopedia letter form: vertices ≥10 use 'A'..'Z' (single char each).
    //   - Mathematica decimal form: vertices ≥10 use multi-digit decimals,
    //     e.g. '910' for vertex-9 connecting to vertex-10.
    // The library's bundled CNIs are in the decimal form.  Disambiguation:
    // each '|'-separated group is the upper-triangular neighbor list of one
    // vertex; for the i-th group, every neighbor j must satisfy
    // groupIdx < j ≤ V-1, where V is the total vertex count (= number of
    // groups).  We greedily take the longest digit prefix that fits.
    //
    // Pre-fix this routine tokenized char-by-char, so '910' became [9,1,0]
    // and '11' became [1,1], producing a bogus over-edged graph for any
    // ≥10-vertex topology.  See commit f9a77b1f8 for the parallel Python
    // fix in scripts/quarantine_unphysical.py.
    let V = (str.match(/\|/g) || []).length;
    if (str.length > 0 && str[str.length - 1] !== '|') V++;

    const nickel = [];
    let accum = [];
    let i = 0;
    let groupIdx = 0;
    while (i < str.length) {
      const c = str[i];
      if (c === '|') {
        nickel.push(accum);
        accum = [];
        groupIdx++;
        i++;
      } else if (CHAR_TO_NODE.has(c)) {
        // Named single-char nodes: 'e' (LEG), 'A'..'Z' (vertex 10..35).
        accum.push(CHAR_TO_NODE.get(c));
        i++;
      } else if (c >= '0' && c <= '9') {
        let runEnd = i;
        while (runEnd < str.length && str[runEnd] >= '0' && str[runEnd] <= '9') runEnd++;
        const runLen = runEnd - i;
        const minV = groupIdx + 1;
        const maxV = Math.max(V - 1, 0);
        let consumed = 0;
        let value = -1;
        for (let len = Math.min(runLen, 3); len >= 1; len--) {
          const v = parseInt(str.substr(i, len), 10);
          if (v >= minV && v <= maxV) {
            consumed = len;
            value = v;
            break;
          }
        }
        if (consumed === 0) {
          // Constraint unsatisfiable (e.g. degenerate input).  Fall back to
          // single-digit interpretation, preserving prior parser behavior.
          value = parseInt(c, 10);
          consumed = 1;
        }
        accum.push(value);
        i += consumed;
      } else {
        // Unknown char — skip (preserves prior tolerance for stray chars).
        i++;
      }
    }
    if (accum.length > 0) nickel.push(accum);
    return nickel;
  }

  static nickelToAdjacent(nickelList) {
    const adjacent = {};
    const backwardNodes = {};
    for (let nodeId = 0; nodeId < nickelList.length; nodeId++) {
      if (!(nodeId in backwardNodes)) backwardNodes[nodeId] = [];
      const forwardNodes = nickelList[nodeId];
      for (const fn of forwardNodes) {
        if (!(fn in backwardNodes)) backwardNodes[fn] = [];
        backwardNodes[fn].push(nodeId);
      }
      adjacent[nodeId] = backwardNodes[nodeId].slice();
      adjacent[nodeId].push(...forwardNodes);
    }
    for (const node in adjacent) {
      adjacent[node].sort((a, b) => a - b);
    }
    return adjacent;
  }
}

// ─── Expander class ──────────────────────────────────────────────────

class Expander {
  constructor(edges, nickelList, nodeMap, currNode, freeNode) {
    this.edges = edges;
    this.nickelList = nickelList;  // Nested lists, each sorted.
    this.nodeMap = nodeMap;        // Map: original node → canonical label
    this.currNode = currNode;
    this.freeNode = freeNode;
  }

  /** Compare two Expanders by their (truncated) Nickel lists. */
  compareTo(other) {
    const minLen = Math.min(this.nickelList.length, other.nickelList.length);
    return compareNickelLists(
      this.nickelList.slice(0, minLen),
      other.nickelList.slice(0, minLen)
    );
  }

  /** Yield all expansions of the current node. */
  *expand() {
    const nodes = adjacentNodes(this.currNode, this.edges);
    const edgeRest = this.edges.filter(e => e[0] !== this.currNode && e[1] !== this.currNode);

    // New nodes: those adjacent but not yet assigned a canonical label.
    const newNodesSet = new Set();
    for (const n of nodes) {
      if (n > this.freeNode) newNodesSet.add(n);
    }
    const newNodes = Array.from(newNodesSet);
    const freeNodes = [];
    for (let i = this.freeNode; i < this.freeNode + newNodes.length; i++) {
      freeNodes.push(i);
    }

    for (const perm of permutations(freeNodes)) {
      const localMap = new Map();
      for (let i = 0; i < newNodes.length; i++) {
        localMap.set(newNodes[i], perm[i]);
      }
      const expandedNodes = mapNodes1(localMap, nodes);
      const mappedEdges = mapNodes2(localMap, edgeRest);
      const combinedMap = new Map(this.nodeMap);
      for (const [k, v] of localMap) combinedMap.set(k, v);

      yield new Expander(
        mappedEdges,
        this.nickelList.concat([expandedNodes]),
        combinedMap,
        this.currNode + 1,
        this.freeNode + newNodes.length
      );
    }

  }
}

// ─── Canonicalize ────────────────────────────────────────────────────

/**
 * Find the canonical Nickel index of a graph given as an edge list.
 *
 * Negative node values are external legs. Non-negative are internal vertices.
 *
 * Usage:
 *   const c = canonicalize([[-1, 10], [-1, 11], [10, 11]]);
 *   c.nickel       // [[-1, 1], [-1]]
 *   c.string       // 'e1|e|'
 *   c.numSymmetries // 2
 *   c.nodeMaps     // [Map(10→0, 11→1), Map(10→1, 11→0)]
 */
function canonicalize(edges) {
  if (!isConnected(edges)) {
    throw new Error('Input edge list is an unconnected graph.');
  }

  // Count internal nodes.
  let numInternalNodes = 0;
  const allNodes = new Set(flatten(edges));
  for (const n of allNodes) {
    if (n >= 0) numInternalNodes++;
  }

  // Shift original nodes to free space for canonical numbers.
  const offset = Math.max(100, numInternalNodes);
  function shift(n) { return n >= 0 ? n + offset : LEG; }
  const shiftedEdges = edges.map(([a, b]) => [shift(a), shift(b)]);

  // Initialize candidate states: one per possible starting node.
  const allShifted = new Set(flatten(shiftedEdges));
  let boundaryNodes;
  if (allShifted.has(LEG)) {
    boundaryNodes = new Set(adjacentNodes(LEG, shiftedEdges));
  } else {
    boundaryNodes = new Set([...allShifted].filter(n => n !== LEG));
  }

  let states = [];
  for (const node of boundaryNodes) {
    const nodeMap = new Map([[node, 0]]);
    states.push(new Expander(
      mapNodes2(nodeMap, shiftedEdges),
      [],
      nodeMap,
      0,
      1
    ));
  }

  // Expand one node at a time, pruning non-minimal states.
  for (let i = 0; i < numInternalNodes; i++) {
    let nextStates = [];
    for (const s of states) {
      for (const expanded of s.expand()) {
        nextStates.push(expanded);
      }
    }
    // Keep only the lexicographically minimum states.
    let minimum = nextStates[0];
    for (let j = 1; j < nextStates.length; j++) {
      if (nextStates[j].compareTo(minimum) < 0) {
        minimum = nextStates[j];
      }
    }
    states = nextStates.filter(s => s.compareTo(minimum) === 0);
  }

  // Collect results.
  const nickelList = states[0].nickelList;
  const numSymmetries = states.length;

  // Verify all surviving states have the same Nickel list.
  for (const s of states) {
    if (compareNickelLists(s.nickelList, nickelList) !== 0) {
      throw new Error('Internal error: surviving states differ.');
    }
  }

  // Shift node maps back to original labels.
  const nodeMaps = states.map(s => {
    const m = new Map();
    for (const [k, v] of s.nodeMap) {
      m.set(k - offset, v);
    }
    return m;
  });

  const nickelStr = Nickel.stringFromNickel(nickelList);

  return {
    nickel: nickelList,
    string: nickelStr,
    numSymmetries,
    nodeMaps,
  };
}

// ─── Exports ─────────────────────────────────────────────────────────

export { Nickel, canonicalize, LEG, isConnected, adjacentNodes };
