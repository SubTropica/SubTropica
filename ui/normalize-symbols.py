#!/usr/bin/env python3
"""
Normalize symbol alphabets in SubTropica library entries.

For each library result with a symbol alphabet (>=2 letters, standard notation):
1. Determine the normalization scale (internal mass > external mass > Mandelstam)
2. Assign W_i labels to non-trivial normalized letters
3. Rewrite symbolTeX with W labels, dropping terms with trivial (scale) letters
4. Store normalization metadata in the entry JSON

Usage:
    python3 normalize-symbols.py [--dry-run]
"""

import json
import os
import re
import sys
from pathlib import Path

LIBRARY_DIR = Path('library-bundled')

# === Regex patterns for atomic variables ===
INTERNAL_MASS_PAT = re.compile(r'^m(?:_(\d+))?\^2$')
EXTERNAL_MASS_PAT = re.compile(r'^M(?:_(\d+))?\^2$')
MANDELSTAM_PAT = re.compile(r'^s_\{(\d+)\}$')

# ============================================================
# Classification
# ============================================================

def is_exotic_alphabet(alphabet):
    """Check if alphabet uses non-standard (parametric) notation."""
    for letter in alphabet:
        if '\\text{' in letter or '\\sqrt{' in letter:
            return True
        if re.search(r'(?:^|[^a-zA-Z\\])([abz])(?:$|[^a-zA-Z_{])', letter):
            return True
        if letter.strip() == 's':
            return True
        if re.search(r'[mM](?:_\d+)?\^1(?!\d)', letter):
            return True
    return False

# ============================================================
# Scale detection
# ============================================================

def find_scale(alphabet, stcommand=''):
    """Determine normalization scale from alphabet and stCommand."""
    # 1. Standalone atomic letters in alphabet
    int_masses = sorted(l.strip() for l in alphabet if INTERNAL_MASS_PAT.match(l.strip()))
    ext_masses = sorted(l.strip() for l in alphabet if EXTERNAL_MASS_PAT.match(l.strip()))
    mandelstams = sorted(l.strip() for l in alphabet if MANDELSTAM_PAT.match(l.strip()))

    if int_masses:
        return int_masses[0]
    if ext_masses:
        return ext_masses[0]

    # 2. Parse stCommand for masses not directly in alphabet
    if stcommand:
        cmd_int, cmd_ext = set(), set()
        for m in re.finditer(r'(?<![a-zA-Z])m(?:\[(\d+)\])?(?=\s*[},\]])', stcommand):
            idx = m.group(1)
            if idx:
                cmd_int.add(f'm_{{{idx}}}^2' if len(idx) > 1 else f'm_{idx}^2')
            else:
                cmd_int.add('m^2')
        for m in re.finditer(r'(?<![a-zA-Z])M(?:\[(\d+)\])?(?=\s*[},\]])', stcommand):
            idx = m.group(1)
            if idx:
                cmd_ext.add(f'M_{{{idx}}}^2' if len(idx) > 1 else f'M_{idx}^2')
            else:
                cmd_ext.add('M^2')
        if cmd_int:
            return sorted(cmd_int)[0]
        if cmd_ext:
            return sorted(cmd_ext)[0]

    # 3. Mandelstam from alphabet (standalone)
    if mandelstams:
        return mandelstams[0]

    # 4. Mandelstam extracted from composite letters
    all_mandel = set()
    for letter in alphabet:
        for m in re.finditer(r's_\{(\d+)\}', letter):
            all_mandel.add(m.group(0))
    if all_mandel:
        return sorted(all_mandel)[0]

    return None

# ============================================================
# Sign-duplicate detection
# ============================================================

def flip_top_level_signs(s):
    """Flip all top-level + and - signs in a TeX expression (respecting brace depth)."""
    result = []
    depth = 0
    for c in s:
        if c == '{':
            depth += 1; result.append(c)
        elif c == '}':
            depth -= 1; result.append(c)
        elif depth == 0 and c == '+':
            result.append('-')
        elif depth == 0 and c == '-':
            result.append('+')
        else:
            result.append(c)
    return ''.join(result)


def canonical_form(tex_nospaces):
    """Canonicalize by sorting top-level additive terms alphabetically."""
    terms = []
    current = ''
    depth = 0
    for c in tex_nospaces:
        if c == '{':
            depth += 1; current += c
        elif c == '}':
            depth -= 1; current += c
        elif depth == 0 and c in '+-' and current:
            terms.append(current)
            current = c
        else:
            current += c
    if current:
        terms.append(current)
    # Ensure leading sign
    normed = []
    for t in terms:
        if t and t[0] not in '+-':
            t = '+' + t
        normed.append(t)
    normed.sort()
    result = ''.join(normed)
    return result.lstrip('+')


def are_negatives(a, b):
    """Check if two TeX letter expressions are negatives of each other."""
    a_c = a.replace(' ', '')
    b_c = b.replace(' ', '')
    if not a_c or not b_c:
        return False
    # Ensure leading sign
    if a_c[0] not in '+-':
        a_c = '+' + a_c
    neg_a = flip_top_level_signs(a_c)
    return canonical_form(neg_a) == canonical_form(b_c)

# ============================================================
# W-label assignment
# ============================================================

def make_w_label(i):
    return f'W_{{{i}}}' if i >= 10 else f'W_{i}'


def make_definition_tex(letter, scale):
    """Create TeX for the definition W_i = letter/scale."""
    letter = letter.strip()
    # Check if letter has top-level + or - (i.e. is composite)
    depth = 0
    for j, c in enumerate(letter):
        if c == '{': depth += 1
        elif c == '}': depth -= 1
        elif depth == 0 and c in '+-' and j > 0:
            return f'({letter})/{scale}'
    if letter.startswith('-'):
        return f'({letter})/{scale}'
    return f'{letter}/{scale}'


def build_w_mapping(alphabet, scale):
    """Build mapping from each alphabet letter to its W label (or None = scale).

    Returns (letter_to_w, definitions).
    """
    letter_to_w = {}
    definitions = []
    representatives = []   # (letter, w_label)

    for letter in alphabet:
        letter = letter.strip()
        if letter == scale:
            letter_to_w[letter] = None
            continue
        if letter in letter_to_w:
            continue
        # Check if this is the negative of an existing W
        found = False
        for rep_letter, w_label in representatives:
            if are_negatives(letter, rep_letter):
                letter_to_w[letter] = w_label
                found = True
                break
        if not found:
            idx = len(representatives) + 1
            label = make_w_label(idx)
            representatives.append((letter, label))
            letter_to_w[letter] = label
            definitions.append({
                'label': label,
                'definition': make_definition_tex(letter, scale),
                'originalLetter': letter,
            })
    return letter_to_w, definitions

# ============================================================
# Symbol TeX rewriting
# ============================================================

def split_terms(tex):
    """Split symbol terms at top-level ' + ' or ' - ' separators.

    Returns list of strings.  The first has no leading separator;
    subsequent ones start with '+ ' or '- '.
    """
    terms = []
    current_start = 0
    depth = 0
    i = 0
    while i < len(tex):
        c = tex[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        elif depth == 0 and i + 2 < len(tex):
            seg = tex[i:i + 3]
            if seg in (' + ', ' - '):
                terms.append(tex[current_start:i])
                current_start = i + 1        # keep +/- as start of next term
                i += 1
                continue
        i += 1
    terms.append(tex[current_start:])
    return terms


def find_letter_group(term):
    """Locate the (letters) group at the end of a term.

    Returns (prefix, letter_content, had_comma) or None.
    """
    stripped = term.rstrip()
    if not stripped.endswith(')'):
        return None
    depth = 0
    for i in range(len(stripped) - 1, -1, -1):
        if stripped[i] == ')':
            depth += 1
        elif stripped[i] == '(':
            depth -= 1
            if depth == 0:
                content = stripped[i + 1:-1]
                prefix = stripped[:i]
                had_comma = prefix.endswith('\\,')
                if had_comma:
                    prefix = prefix[:-2]
                return prefix, content, had_comma
    return None


def process_term(term, letter_to_w):
    """Replace letters with W labels.  Returns (new_term, should_drop)."""
    result = find_letter_group(term)
    if result is None:
        return term, False
    prefix, content, had_comma = result
    parts = [p.strip() for p in content.split(' \\otimes ')]

    has_scale = False
    new_parts = []
    for part in parts:
        # Exact match
        if part in letter_to_w:
            w = letter_to_w[part]
            if w is None:
                has_scale = True; break
            new_parts.append(w); continue
        # Whitespace-insensitive match
        matched = False
        for known, w in letter_to_w.items():
            if part.replace(' ', '') == known.replace(' ', ''):
                if w is None:
                    has_scale = True; break
                new_parts.append(w); matched = True; break
        if has_scale:
            break
        if not matched:
            new_parts.append(part)       # keep unrecognised letters as-is

    if has_scale:
        return term, True

    comma = '\\,' if had_comma else ''
    return prefix + comma + '(' + ' \\otimes '.join(new_parts) + ')', False


def rewrite_terms(terms_tex, letter_to_w):
    """Rewrite a single order's terms string."""
    terms = split_terms(terms_tex)
    surviving = []
    for t in terms:
        new_t, drop = process_term(t, letter_to_w)
        if not drop:
            surviving.append(new_t)
    if not surviving:
        return ''
    # Fix leading separator on first surviving term
    first = surviving[0]
    if first.startswith('+ '):
        surviving[0] = first[2:]
    elif first.startswith('- '):
        surviving[0] = '-' + first[2:]
    return ' '.join(surviving)


def rewrite_symbol_tex(symbol_tex, letter_to_w):
    """Full symbolTeX rewrite: handle multi-order aligned or single-order."""
    if not symbol_tex:
        return ''

    if '\\begin{aligned}' in symbol_tex:
        inner = re.sub(r'^\\begin\{aligned\}', '', symbol_tex)
        inner = re.sub(r'\\end\{aligned\}$', '', inner)
        orders = inner.split(' \\\\ ')
        new_orders = []
        for order in orders:
            pfx = re.match(r'(\\varepsilon\^\{[^}]*\}&\\colon\\;)', order)
            if pfx:
                prefix = pfx.group(1)
                terms_tex = order[len(prefix):]
            else:
                prefix = ''
                terms_tex = order
            new_terms = rewrite_terms(terms_tex, letter_to_w)
            if new_terms:
                new_orders.append(prefix + new_terms)
        if not new_orders:
            return ''
        if len(new_orders) == 1:
            return new_orders[0]
        return '\\begin{aligned}' + ' \\\\ '.join(new_orders) + '\\end{aligned}'

    # Single order
    pfx = re.match(r'(\\varepsilon\^\{[^}]*\}(?:&)?\\colon\\;)', symbol_tex)
    if pfx:
        prefix = pfx.group(1)
        terms_tex = symbol_tex[len(prefix):]
    else:
        prefix = ''
        terms_tex = symbol_tex
    new_terms = rewrite_terms(terms_tex, letter_to_w)
    return (prefix + new_terms) if new_terms else ''

# ============================================================
# Entry processing
# ============================================================

def normalize_result(result):
    """Normalize one result.  Returns dict of new fields or None."""
    alphabet = result.get('alphabet', [])
    symbol_tex = result.get('symbolTeX', '')
    stcommand = result.get('stCommand', '')

    if len(alphabet) < 2 or not symbol_tex:
        return None
    if is_exotic_alphabet(alphabet):
        return None

    scale = find_scale(alphabet, stcommand)
    if not scale:
        return None

    letter_to_w, definitions = build_w_mapping(alphabet, scale)
    if not definitions:
        return None

    normalized_tex = rewrite_symbol_tex(symbol_tex, letter_to_w)

    # Count surviving terms
    norm_terms = 0
    for d in definitions:
        norm_terms += normalized_tex.count(d['label'])
    # More accurate: count \otimes-separated slots
    # Simple heuristic: count W_ occurrences not inside definitions
    # Actually just count ( groups
    norm_term_count = len(re.findall(r'(?:^|[^a-zA-Z])W_', normalized_tex))
    # Better: count opening parens that follow \, or start a letter group
    paren_count = normalized_tex.count('\\,(') + normalized_tex.count('+ (') + normalized_tex.count('- (')
    if normalized_tex and normalized_tex[0] == '(':
        paren_count += 1
    # Fallback to simple regex
    # Count terms by finding all ( ... ) groups containing W_
    norm_term_count = len(re.findall(r'\((?:[^)]*W_[^)]*)\)', normalized_tex))

    return {
        'symbolScale': scale,
        'wDefinitions': definitions,
        'normalizedAlphabet': [d['label'] for d in definitions],
        'normalizedSymbolTeX': normalized_tex,
        'normalizedSymbolTerms': norm_term_count,
    }

# ============================================================
# Main
# ============================================================

def main():
    dry_run = '--dry-run' in sys.argv
    verbose = '--verbose' in sys.argv or dry_run

    processed = 0
    skipped_exotic = 0
    skipped_small = 0
    skipped_no_scale = 0
    errors = 0

    for root, dirs, files in sorted(os.walk(LIBRARY_DIR)):
        dirs.sort()
        if 'entry.json' not in files:
            continue
        entry_path = os.path.join(root, 'entry.json')
        with open(entry_path) as f:
            entry = json.load(f)

        modified = False
        for result in entry.get('Results', []):
            alphabet = result.get('alphabet', [])
            if len(alphabet) < 2:
                if alphabet:
                    skipped_small += 1
                continue

            try:
                new_fields = normalize_result(result)
            except Exception as e:
                print(f'ERROR {entry_path}: {e}', file=sys.stderr)
                errors += 1
                continue

            if new_fields is None:
                if is_exotic_alphabet(alphabet):
                    skipped_exotic += 1
                else:
                    skipped_no_scale += 1
                continue

            if verbose:
                rel = os.path.relpath(entry_path, LIBRARY_DIR)
                print(f'{rel}:')
                print(f'  scale = {new_fields["symbolScale"]}')
                for d in new_fields['wDefinitions']:
                    print(f'  {d["label"]} = {d["definition"]}')
                print(f'  terms: {result.get("symbolTerms",0)} -> {new_fields["normalizedSymbolTerms"]}')
                if verbose and dry_run:
                    orig = result.get('symbolTeX', '')
                    print(f'  ORIG:  {orig[:120]}...')
                    print(f'  NORM:  {new_fields["normalizedSymbolTeX"][:120]}...')
                print()

            result.update(new_fields)
            modified = True
            processed += 1

        if modified and not dry_run:
            with open(entry_path, 'w') as f:
                json.dump(entry, f, indent=2, ensure_ascii=False)
                f.write('\n')

    print(f'Processed:         {processed}')
    print(f'Skipped (exotic):  {skipped_exotic}')
    print(f'Skipped (<2 let):  {skipped_small}')
    print(f'Skipped (no scale):{skipped_no_scale}')
    print(f'Errors:            {errors}')
    if dry_run:
        print('(dry run — no files modified)')


if __name__ == '__main__':
    main()
