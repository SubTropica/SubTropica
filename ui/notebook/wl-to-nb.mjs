// Convert a rendered SubTropica .wl template into a Mathematica .nb notebook.
//
// Key insight (verified by round-trip through the real Mathematica front end):
// `Cell["source code", "Input"]` IS evaluatable. Mathematica reparses the
// string into boxes when the cell is rendered, so Shift-Enter works and
// produces proper Out[] cells. That means a pure-JS .nb writer — no
// Mathematica kernel needed at generation time — can emit perfectly usable
// notebooks. (We *thought* this didn't work earlier; the misread was that
// `.wl` files open in Package Editor, which has different semantics.)
//
// Recognized cell patterns in the source .wl:
//
//   (* ::Title::         *)  (* body *)   →  Cell["body", "Title"]
//   (* ::Section::       *)  (* body *)   →  Cell["body", "Section"]
//   (* ::Subsection::    *)  (* body *)   →  Cell["body", "Subsection"]
//   (* ::Subsubsection:: *)  (* body *)   →  Cell["body", "Subsubsection"]
//   (* ::Text::          *)  (* body *)   →  Cell["body", "Text"]
//   (* ::Program::       *)  (* body *)   →  Cell["body", "Program"]
//   (* ::Code::          *)  (* body *)   →  Cell["body", "Code"]
//   <raw code>                            →  Cell["code", "Input"]

const STYLE_MARKERS = new Set([
  'Title', 'Chapter', 'Section', 'Subsection', 'Subsubsection',
  'Text', 'Program', 'Code', 'Item',
  // Pre-rendered diagram: body is a raw GraphicsBox (Mathematica boxes, NOT a
  // string), emitted as Cell[BoxData[...], "Output"] so it displays on open.
  'DiagramGraphics',
]);

// Mathematica .nb files expect ASCII-safe string content: non-ASCII characters
// must be escaped as \:XXXX (hex codepoint), or the FE interprets raw UTF-8
// bytes as Latin-1 and you see things like "â" where an em-dash used to be.
function escapeForMmaString(s) {
  return s
    .replace(/\\/g, '\\\\')
    .replace(/"/g, '\\"')
    .replace(/[\u0080-\uffff]/g, c =>
      '\\:' + c.charCodeAt(0).toString(16).padStart(4, '0')
    );
}

export function parseCells(wl) {
  const cells = [];
  const lines = wl.split('\n');
  let i = 0;

  const isStyleMarker = line => {
    const m = line.match(/^\(\* ::(\w+):: \*\)\s*$/);
    return m && STYLE_MARKERS.has(m[1]) ? m[1] : null;
  };

  const skipBlank = () => {
    while (i < lines.length && lines[i].trim() === '') i++;
  };

  while (i < lines.length) {
    const style = isStyleMarker(lines[i]);

    if (style) {
      i++;
      skipBlank();

      if (i >= lines.length) break;
      const first = lines[i];
      if (first.startsWith('(*')) {
        // Read a (possibly multi-line, possibly nested) comment body. Track
        // (* / *) nesting depth so an inner comment like `(* note *)` does not
        // prematurely terminate the cell. Without this, a Program example whose
        // body contains its own comment was split into a truncated Program cell
        // plus an evaluatable Input cell ending in a dangling `*)` (syntax
        // error). A single-line `(* x *)` has net depth 0, so the loop is a
        // no-op and it is handled by the same path.
        const depthOf = s =>
          (s.match(/\(\*/g) || []).length - (s.match(/\*\)/g) || []).length;
        let raw = first;
        let depth = depthOf(first);
        i++;
        while (i < lines.length && depth > 0) {
          raw += '\n' + lines[i];
          depth += depthOf(lines[i]);
          i++;
        }
        const body = raw.replace(/^\(\*/, '').replace(/\*\)\s*$/, '');
        cells.push({ style, text: body.trim() });
      } else {
        cells.push({ style, text: '' });
      }
      continue;
    }

    // Raw code block until the next style marker. Commented-out code blocks
    // like `(* ConfigureSubTropica[...] *)` are kept verbatim as Input cells
    // so users can uncomment to run.
    const codeLines = [];
    while (i < lines.length && !isStyleMarker(lines[i])) {
      codeLines.push(lines[i]);
      i++;
    }
    while (codeLines.length && codeLines[codeLines.length - 1].trim() === '') {
      codeLines.pop();
    }
    while (codeLines.length && codeLines[0].trim() === '') {
      codeLines.shift();
    }
    const code = codeLines.join('\n');
    if (!code.trim()) continue;
    if (/^\(\* ::Package:: \*\)\s*$/.test(code)) continue;
    cells.push({ style: 'Input', text: code });
  }

  return cells;
}

// Styles whose cells should fire on Shift-Enter. The Input/Code styles are
// Evaluatable by default in Mathematica's Default.nb, but we set it explicitly
// so the behavior survives any custom stylesheet (including our Terra Verde
// sheet below) without relying on style inheritance.
const EVALUATABLE_STYLES = new Set(['Input', 'Code']);

// Heading nesting levels. A heading groups every following cell that is
// strictly deeper than it, until the next heading at its own level or shallower.
const HEADING_LEVEL = { Title: 0, Chapter: 1, Section: 2, Subsection: 3, Subsubsection: 4 };

// Nest the flat cell list into CellGroupData groups by heading level, so each
// section folds/unfolds from its cell bracket in the front end. Every group is
// emitted Open, so the notebook opens fully expanded.
function groupCells(cells) {
  let i = 0;
  const levelOf = c => (c.style in HEADING_LEVEL ? HEADING_LEVEL[c.style] : Infinity);
  // Collect cells strictly deeper than `parentLevel` as node expressions.
  function build(parentLevel) {
    const out = [];
    while (i < cells.length) {
      const c = cells[i];
      const lvl = levelOf(c);
      if (lvl <= parentLevel) break;            // belongs to an ancestor heading
      if (lvl === Infinity) {                    // leaf content cell
        out.push(cellToExpr(c));
        i++;
      } else {                                   // heading: group with its descendants
        i++;
        const children = build(lvl);
        out.push(children.length
          ? `Cell[CellGroupData[{\n${[cellToExpr(c), ...children].join(',\n')}\n}, Open]]`
          : cellToExpr(c));
      }
    }
    return out;
  }
  return build(-1);
}

function cellToExpr(cell) {
  // Pre-rendered diagram: the body is raw Mathematica boxes (a GraphicsBox),
  // not a string. Emit it verbatim inside BoxData so the front end renders the
  // vector graphic on open. Do NOT escape (it is boxes, not string content).
  if (cell.style === 'DiagramGraphics') {
    return `Cell[BoxData[${cell.text}], "Output"]`;
  }
  const base = `Cell["${escapeForMmaString(cell.text)}", "${cell.style}"`;
  if (EVALUATABLE_STYLES.has(cell.style)) {
    return `${base}, Evaluatable->True]`;
  }
  return `${base}]`;
}

// SubTropica "Terra Verde" theme, ported from ui/style.css. Inlined in each
// notebook so the theme travels with the file and works offline.
const TERRA_VERDE_STYLESHEET = `StyleDefinitions->Notebook[{
Cell[StyleData["Notebook"],
  Background->RGBColor[0.98039, 0.96078, 0.93333]],
Cell[StyleData["Title"],
  FontFamily->"DM Sans",
  FontSize->32, FontWeight->"Bold",
  FontColor->RGBColor[0.65882, 0.27059, 0.27059],
  CellMargins->{{66, 10}, {14, 28}}],
Cell[StyleData["Section"],
  FontFamily->"DM Sans",
  FontSize->20, FontWeight->"Bold",
  FontColor->RGBColor[0.65882, 0.27059, 0.27059],
  CellMargins->{{66, 10}, {10, 22}}],
Cell[StyleData["Subsection"],
  FontFamily->"DM Sans",
  FontSize->15, FontWeight->"Bold",
  FontColor->RGBColor[0.23922, 0.20392, 0.15686],
  CellMargins->{{70, 10}, {8, 16}}],
Cell[StyleData["Text"],
  FontFamily->"DM Sans",
  FontSize->13,
  FontColor->RGBColor[0.23922, 0.20392, 0.15686],
  LineSpacing->{1.3, 0},
  CellMargins->{{70, 30}, {6, 8}}],
Cell[StyleData["Input"],
  FontFamily->"JetBrains Mono",
  FontSize->12,
  FontColor->RGBColor[0.23922, 0.20392, 0.15686],
  Background->RGBColor[0.99216, 0.97647, 0.95686],
  CellFrame->0.5,
  CellFrameColor->RGBColor[0.88627, 0.84706, 0.78431],
  CellFrameMargins->{{8, 8}, {4, 4}},
  CellMargins->{{66, 20}, {6, 10}}],
Cell[StyleData["Output"],
  FontFamily->"JetBrains Mono",
  FontSize->12,
  FontColor->RGBColor[0.23922, 0.20392, 0.15686],
  CellMargins->{{66, 20}, {6, 14}}],
Cell[StyleData["Program"],
  FontFamily->"JetBrains Mono",
  FontSize->11,
  FontColor->RGBColor[0.42, 0.35, 0.25],
  Background->RGBColor[0.95686, 0.93333, 0.89412],
  CellFrame->0.5,
  CellFrameColor->RGBColor[0.88627, 0.84706, 0.78431],
  CellFrameMargins->{{8, 8}, {4, 4}},
  CellMargins->{{70, 20}, {4, 8}}]
}, StyleDefinitions->"Default.nb"]`;

export function wlToNb(wl, {
  creator = 'subtropi.ca notebook generator',
  theme = 'terra-verde',
} = {}) {
  const cells = parseCells(wl);
  const header =
    '(* Content-type: application/vnd.wolfram.mathematica *)\n' +
    '\n' +
    '(*** Wolfram Notebook File ***)\n' +
    '(* http://www.wolfram.com/nb *)\n' +
    '\n' +
    `(* CreatedBy='${creator}' *)\n` +
    '\n';

  const styleDefs = theme === 'terra-verde'
    ? TERRA_VERDE_STYLESHEET
    : 'StyleDefinitions->"Default.nb"';

  const body =
    'Notebook[{\n' +
    groupCells(cells).join(',\n\n') +
    '\n},\n' +
    // Manual grouping: honor the explicit CellGroupData we emit. With the
    // default Automatic grouping the front end re-derives groups from each
    // heading style's CellGroupingRules — but our custom Terra Verde stylesheet
    // overrides Title/Section/Subsection without those rules, so Automatic
    // silently dissolves every group (sections stop folding). Manual makes the
    // explicit groups authoritative, so the notebook opens with foldable,
    // fully-expanded sections.
    'CellGrouping->Manual,\n' +
    'WindowSize->{960, 820},\n' +
    'WindowMargins->{{Automatic, 80}, {Automatic, 40}},\n' +
    styleDefs + '\n' +
    ']\n';

  return header + body;
}
