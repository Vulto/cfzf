#!/usr/bin/env python3
"""Minimal ANSI screen emulator.

Renders a captured terminal byte stream into a logical screen (rows of
cells), so that two different programs' output can be compared as screen
states rather than raw bytes. Supports the subset of sequences fzf emits:
CUP/HVP, CUU/CUD/CUF/CUB, ED, EL, SGR (ignored), DECSC/DECRC, and plain
text with wrapping.
"""
import re
import sys


class Screen:
    def __init__(self, cols=80, rows=24):
        self.cols = cols
        self.rows = rows
        self.buf = [[" "] * cols for _ in range(rows)]
        # The foreground colour of every cell, so that the current line and
        # the colour attributes can be compared as well.
        self.colors = [[None] * cols for _ in range(rows)]
        self.fg = None
        self.x = 0
        self.y = 0
        self.saved = (0, 0)

    def _clear(self):
        self.buf = [[" "] * self.cols for _ in range(self.rows)]

    def _put(self, ch):
        if self.y < 0 or self.y >= self.rows:
            return
        if self.x >= self.cols:
            self.x = 0
            self.y += 1
            if self.y >= self.rows:
                return
        if 0 <= self.y < self.rows and 0 <= self.x < self.cols:
            self.buf[self.y][self.x] = ch
            self.colors[self.y][self.x] = self.fg
        self.x += 1

    def feed(self, data):
        text = data.decode("utf-8", "replace")
        i = 0
        n = len(text)
        while i < n:
            ch = text[i]
            if ch == "\x1b":
                m = re.match(r"\x1b\[([0-9;?]*)([ -/]*)([@-~])", text[i:])
                if m:
                    self._csi(m.group(1), m.group(3))
                    i += m.end()
                    continue
                m = re.match(r"\x1b\]([^\x07\x1b]*)(\x07|\x1b\\)", text[i:])
                if m:
                    i += m.end()
                    continue
                m = re.match(r"\x1b([()][A-Za-z0-9])", text[i:])
                if m:
                    i += m.end()
                    continue
                if text.startswith("\x1b7", i):
                    self.saved = (self.x, self.y)
                    i += 2
                    continue
                if text.startswith("\x1b8", i):
                    self.x, self.y = self.saved
                    i += 2
                    continue
                # Unknown sequence: skip the escape and the next byte.
                i += 2
                continue
            if ch == "\r":
                self.x = 0
                i += 1
                continue
            if ch == "\n":
                self.y += 1
                if self.y >= self.rows:
                    self.y = self.rows - 1
                i += 1
                continue
            if ch == "\b":
                self.x = max(0, self.x - 1)
                i += 1
                continue
            if ch == "\x07":
                i += 1
                continue
            self._put(ch)
            i += 1
        return self

    def _csi(self, params, final):
        nums = []
        for p in params.split(";"):
            if p.isdigit():
                nums.append(int(p))
            elif p == "":
                nums.append(0)
            else:
                nums.append(0)
        if not nums:
            nums = [0]

        def arg(idx, default=1):
            if idx < len(nums) and nums[idx] != 0:
                return nums[idx]
            if idx < len(nums) and nums[idx] == 0 and default == 0:
                return 0
            return default

        if final == "H" or final == "f":
            row = nums[0] if len(nums) > 0 and nums[0] > 0 else 1
            col = nums[1] if len(nums) > 1 and nums[1] > 0 else 1
            self.y = min(self.rows - 1, max(0, row - 1))
            self.x = min(self.cols - 1, max(0, col - 1))
        elif final == "A":
            self.y = max(0, self.y - arg(0))
        elif final == "B":
            self.y = min(self.rows - 1, self.y + arg(0))
        elif final == "C":
            self.x = min(self.cols - 1, self.x + arg(0))
        elif final == "D":
            self.x = max(0, self.x - arg(0))
        elif final == "G":
            self.x = min(self.cols - 1, max(0, arg(0) - 1))
        elif final == "d":
            self.y = min(self.rows - 1, max(0, arg(0) - 1))
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 2 or mode == 3:
                self._clear()
            elif mode == 0:
                for x in range(self.x, self.cols):
                    self.buf[self.y][x] = " "
                for y in range(self.y + 1, self.rows):
                    self.buf[y] = [" "] * self.cols
            else:
                self._clear()
        elif final == "K":
            mode = nums[0] if nums else 0
            if mode == 0:
                for x in range(self.x, self.cols):
                    self.buf[self.y][x] = " "
            elif mode == 1:
                for x in range(0, self.x + 1):
                    self.buf[self.y][x] = " "
            else:
                self.buf[self.y] = [" "] * self.cols
        elif final == "L":
            cnt = arg(0)
            for _ in range(cnt):
                self.buf.insert(self.y, [" "] * self.cols)
                del self.buf[self.rows]
        elif final == "M":
            cnt = arg(0)
            for _ in range(cnt):
                del self.buf[self.y]
                self.buf.append([" "] * self.cols)
        elif final == "P":
            cnt = arg(0)
            row = self.buf[self.y]
            del row[self.x:self.x + cnt]
            row.extend([" "] * cnt)
        elif final == "X":
            cnt = arg(0)
            for x in range(self.x, min(self.cols, self.x + cnt)):
                self.buf[self.y][x] = " "
        elif final in ("h", "l"):
            pass  # private modes (alt screen, mouse, cursor visibility)
        elif final == "m":
            self._sgr(params)

    def _sgr(self, params):
        """Track the foreground colour of the following text."""
        values = [p for p in params.split(";") if p != ""]
        i = 0
        while i < len(values):
            p = values[i]
            if p == "0" or p == "":
                self.fg = None
            elif p == "39":
                self.fg = None
            elif p in ("38", "48") and i + 2 < len(values) and values[i + 1] == "5":
                if p == "38":
                    self.fg = values[i + 2]
                i += 2
            elif p.isdigit() and 30 <= int(p) <= 37:
                self.fg = str(int(p) - 30)
            elif p.isdigit() and 90 <= int(p) <= 97:
                self.fg = str(int(p) - 90 + 8)
            i += 1

    def lines(self, strip=True):
        out = []
        for row in self.buf:
            s = "".join(row)
            out.append(s.rstrip() if strip else s)
        while out and out[-1] == "":
            out.pop()
        return out

    def text(self, strip=True):
        return "\n".join(self.lines(strip))

    def colored(self, strip=True):
        """The screen with the foreground colour of every cell appended, so
        that colour differences show up in the comparison."""
        out = []
        for y, row in enumerate(self.buf):
            cells = []
            for x, ch in enumerate(row):
                c = self.colors[y][x]
                cells.append(ch if c is None else "%s:%s" % (ch, c))
            line = "".join(cells)
            out.append(line.rstrip() if strip else line)
        while out and out[-1] == "":
            out.pop()
        return "\n".join(out)


def render(data, cols=80, rows=24):
    return Screen(cols, rows).feed(data).text()


if __name__ == "__main__":
    cols = int(sys.argv[1]) if len(sys.argv) > 1 else 80
    rows = int(sys.argv[2]) if len(sys.argv) > 2 else 24
    data = sys.stdin.buffer.read()
    print(render(data, cols, rows))
