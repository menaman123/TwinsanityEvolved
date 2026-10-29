#!/usr/bin/env python3
"""shape.py -- the text layer's shaping (platform/nativeee/wasm/text_shape.cpp) for the tools: the same rules, step for step,
so a translation's widths are measured as the game will draw it. tools/judges/arabic_length_judge.py checks this module
against the C++ itself (te_text_measure) on every string of the bank.

    shape_text(bank_bytes) -> {"rtl": bool, "lines": [[(codepoint, byte, source), ...] display order], "empty": [line index]}
"""
import arabic_forms as AF

CP1256 = [bytes([b]).decode("cp1256", errors="replace") for b in range(256)]


def cp1256(b):
    return ord(CP1256[b]) if b >= 0x80 else b


def has_arabic(data):
    return any(b >= 0x80 and cp1256(b) in AF.LETTERS or (b >= 0x80 and cp1256(b) == AF.TATWEEL) for b in data)


N, R, L, EN, CS, ET = range(6)


def class_of(cp):
    if 0x0621 <= cp <= 0x064A or 0xFE70 <= cp <= 0xFEFC or cp in (0x060C, 0x061B, 0x061F):
        return R
    if 0x30 <= cp <= 0x39 or 0x0660 <= cp <= 0x0669:
        return EN
    if 0x41 <= cp <= 0x5A or 0x61 <= cp <= 0x7A or (0xC0 <= cp <= 0x24F and cp not in (0xD7, 0xF7)):
        return L
    if cp in (0x2E, 0x2C, 0x3A, 0x2F):
        return CS
    if cp == 0x25:
        return ET
    return N


def shape_line(data, start, end, rtl):
    units = []   # [cp, source, byte, cls, level]
    i = start
    while i < end:
        cp = cp1256(data[i])
        u = [cp, i, data[i], 0, 0]
        if cp in AF.LETTERS or cp == AF.TATWEEL:
            prev = cp1256(data[i - 1]) if i > start else 0
            nxt = cp1256(data[i + 1]) if i + 1 < end else 0
            join_prev = bool(prev) and AF.joins_left(prev) and AF.joins_right(cp)
            if cp == AF.LAM and nxt in AF.LAM_ALEF:
                iso, fin = AF.LAM_ALEF[nxt]
                u[0] = fin if join_prev else iso
                i += 1
            elif cp != AF.TATWEEL:
                cls, base = AF.LETTERS[cp]
                join_next = bool(nxt) and AF.joins_left(cp) and AF.joins_right(nxt)
                if cls == "D":
                    form = 3 if join_prev and join_next else 2 if join_next else 1 if join_prev else 0
                elif cls == "R":
                    form = 1 if join_prev else 0
                else:
                    form = 0
                u[0] = base + form
        u[3] = class_of(u[0])
        units.append(u)
        i += 1
    n = len(units)
    if rtl:
        for k in range(n):
            c = units[k][3]
            if c == CS and 0 < k < n - 1 and units[k - 1][3] == EN and units[k + 1][3] == EN:
                units[k][3] = EN
            elif c == ET and ((k > 0 and units[k - 1][3] == EN) or (k + 1 < n and units[k + 1][3] == EN)):
                units[k][3] = EN
        for u in units:
            if u[3] in (CS, ET):
                u[3] = N
        strong = R
        for u in units:
            if u[3] in (L, R):
                strong = u[3]
            elif u[3] == EN and strong == L:
                u[3] = L
        for u in units:
            if u[3] == EN and 0x30 <= u[0] <= 0x39:
                u[0] = 0x0660 + u[0] - 0x30
        # paired brackets (N0 for ( )): right to left when the pair holds right-to-left text or a number; only Latin inside:
        # the direction before the opening bracket
        stack = []
        for k in range(n):
            if units[k][0] == 0x28 and len(stack) < 64:
                stack.append(k)
            elif units[k][0] == 0x29 and stack:
                o = stack.pop()
                inside = [units[x][3] for x in range(o + 1, k)]
                has_r = any(c in (R, EN) for c in inside)
                has_l = any(c == L for c in inside)
                d = None
                if has_r:
                    d = R
                elif has_l:
                    d = R
                    for x in range(o - 1, -1, -1):
                        if units[x][3] == L:
                            d = L
                            break
                        if units[x][3] in (R, EN):
                            break
                if d is not None:
                    units[o][3] = units[k][3] = d
        k = 0
        while k < n:
            if units[k][3] != N:
                k += 1
                continue
            j = k
            while j < n and units[j][3] == N:
                j += 1
            before = L if k > 0 and units[k - 1][3] == L else R
            after = L if j < n and units[j][3] == L else R
            resolved = before if before == after else R
            for m in range(k, j):
                units[m][3] = resolved
            k = j
        for u in units:
            u[4] = 1 if u[3] == R else 2
        k = n - 1
        while k >= 0 and units[k][0] == 0x20:
            units[k][4] = 1
            k -= 1
        for u in units:
            if u[4] == 1 and u[0] in (0x28, 0x29):
                u[0] = 0x29 if u[0] == 0x28 else 0x28
                u[2] = u[0]
    out = []
    if not rtl:
        out = units
    else:
        k = n - 1
        while k >= 0:
            if units[k][4] == 2:
                s = k
                while s > 0 and units[s - 1][4] == 2:
                    s -= 1
                out += units[s:k + 1]
                k = s - 1
            else:
                out.append(units[k])
                k -= 1
    return [(u[0], u[2], u[1]) for u in out]


def shape_text(data):
    """The text's lines as the game's RenderText splits them ('~' ends a line; a '~' where a line would start is an empty
    line), each shaped and in display order."""
    rtl = has_arabic(data)
    lines, empty = [], []
    pos = 0
    while pos < len(data) and data[pos] != 0:
        if data[pos] == 0x7E:
            empty.append(len(lines))
            lines.append([])
            pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in (0, 0x7E):
            pos += 1
        lines.append(shape_line(data, start, pos, rtl))
        if pos < len(data) and data[pos] == 0x7E:
            pos += 1
    return {"rtl": rtl, "lines": lines, "empty": empty}
