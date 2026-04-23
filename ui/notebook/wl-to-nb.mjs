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

function parseCells(wl) {
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
      const singleLine = first.match(/^\(\*(.*)\*\)\s*$/);
      if (singleLine) {
        cells.push({ style, text: singleLine[1].trim() });
        i++;
      } else if (first.startsWith('(*')) {
        let body = first.replace(/^\(\*/, '');
        i++;
        while (i < lines.length && !lines[i].includes('*)')) {
          body += '\n' + lines[i];
          i++;
        }
        if (i < lines.length) {
          body += '\n' + lines[i].replace(/\*\)\s*$/, '');
          i++;
        }
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

function cellToExpr(cell) {
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
    cells.map(cellToExpr).join(',\n\n') +
    '\n},\n' +
    'WindowSize->{960, 820},\n' +
    'WindowMargins->{{Automatic, 80}, {Automatic, 40}},\n' +
    styleDefs + '\n' +
    ']\n';

  return header + body;
}
