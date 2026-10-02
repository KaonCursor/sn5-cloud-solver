from bisect import bisect_left, bisect_right

MAX = 2 ** 63 - 1
_BLOCK = 512


class _SortedKeys:
    """Bucketed sorted list of distinct ints, each with a value; per-block max of values."""

    def __init__(self):
        self.blocks = []
        self.vals = []
        self.firsts = []
        self.maxes = []

    def _rebuild(self):
        flat = [k for b in self.blocks for k in b]
        fv = [v for b in self.vals for v in b]
        self.blocks = [flat[i:i + _BLOCK] for i in range(0, len(flat), _BLOCK)]
        self.vals = [fv[i:i + _BLOCK] for i in range(0, len(fv), _BLOCK)]
        self.firsts = [b[0] for b in self.blocks]
        self.maxes = [max(b) for b in self.vals]

    def add(self, k, v=0):
        if not self.blocks:
            self.blocks = [[k]]
            self.vals = [[v]]
            self.firsts = [k]
            self.maxes = [v]
            return
        i = bisect_right(self.firsts, k) - 1
        if i < 0:
            i = 0
        b = self.blocks[i]
        j = bisect_left(b, k)
        b.insert(j, k)
        self.vals[i].insert(j, v)
        self.firsts[i] = b[0]
        if v > self.maxes[i]:
            self.maxes[i] = v
        if len(b) > 2 * _BLOCK:
            self._rebuild()

    def remove(self, k):
        i = bisect_right(self.firsts, k) - 1
        b = self.blocks[i]
        j = bisect_left(b, k)
        del b[j]
        vb = self.vals[i]
        v = vb.pop(j)
        if not b:
            del self.blocks[i]
            del self.vals[i]
            del self.firsts[i]
            del self.maxes[i]
            return
        self.firsts[i] = b[0]
        if v == self.maxes[i]:
            self.maxes[i] = max(vb)

    def pred(self, k):
        """Largest key <= k, or None."""
        i = bisect_right(self.firsts, k) - 1
        if i < 0:
            return None
        b = self.blocks[i]
        return b[bisect_right(b, k) - 1]

    def first_fit(self, need):
        """Smallest key whose value >= need, or None."""
        for i, m in enumerate(self.maxes):
            if m >= need:
                vb = self.vals[i]
                for j, v in enumerate(vb):
                    if v >= need:
                        return self.blocks[i][j]
        return None


def extent_journal(chunk_size, address_limit, large_threshold, events):
    cs = chunk_size

    def phys(size):
        if size < large_threshold or size > MAX or size > MAX - (cs - 1):
            return None
        return (size + cs - 1) // cs * cs

    # Free gaps: maximal free intervals [g, gap_end[g]), all nonempty.
    gap_end = {}
    gap_start_of_end = {}
    gaps = _SortedKeys()

    def add_gap(a, b):
        if a < b:
            gap_end[a] = b
            gap_start_of_end[b] = a
            gaps.add(a, b - a)

    def del_gap(a):
        b = gap_end.pop(a)
        del gap_start_of_end[b]
        gaps.remove(a)
        return b

    def is_free(s, e):
        g = gaps.pred(s)
        return g is not None and gap_end[g] >= e

    def occupy(s, e):
        g = gaps.pred(s)
        ge = del_gap(g)
        add_gap(g, s)
        add_gap(e, ge)

    def free(s, e):
        a = s
        b = e
        ls = gap_start_of_end.get(s)
        if ls is not None:
            del_gap(ls)
            a = ls
        if e in gap_end:
            b = del_gap(e)
        add_gap(a, b)

    add_gap(0, address_limit)

    active = {}  # id -> [arena, start, logical, end]
    owner = {}  # start -> id
    starts = _SortedKeys()

    out = []
    for ev in events:
        op = ev[0]
        if op == "claim":
            aid, arena, start, size = ev[1], ev[2], ev[3], ev[4]
            p = None
            if aid not in active and start >= 0 and start % cs == 0:
                p = phys(size)
            if p is None:
                out.append(("rejected",))
                continue
            end = start + p
            if end > address_limit or not is_free(start, end):
                out.append(("rejected",))
                continue
            occupy(start, end)
            active[aid] = [arena, start, size, end]
            owner[start] = aid
            starts.add(start)
            out.append(("claimed", start, end))
        elif op == "resize":
            aid, size = ev[1], ev[2]
            rec = active.get(aid)
            p = phys(size) if rec is not None else None
            if p is None:
                out.append(("rejected",))
                continue
            _, s, _, e = rec
            ne = s + p
            if ne <= e:
                if ne < e:
                    free(ne, e)
                rec[2] = size
                rec[3] = ne
                out.append(("resized", s, e, s, ne))
            elif ne <= address_limit and (e in gap_end and gap_end[e] >= ne):
                occupy(e, ne)
                rec[2] = size
                rec[3] = ne
                out.append(("resized", s, e, s, ne))
            else:
                g = gaps.first_fit(p)
                if g is None:
                    out.append(("rejected",))
                    continue
                occupy(g, g + p)
                free(s, e)
                del owner[s]
                starts.remove(s)
                owner[g] = aid
                starts.add(g)
                rec[1] = g
                rec[2] = size
                rec[3] = g + p
                out.append(("resized", s, e, g, g + p))
        elif op == "release":
            aid = ev[1]
            rec = active.pop(aid, None)
            if rec is None:
                out.append(("rejected",))
                continue
            _, s, _, e = rec
            free(s, e)
            del owner[s]
            starts.remove(s)
            out.append(("released", s, e))
        elif op == "lookup":
            addr = ev[1]
            st = starts.pred(addr)
            if st is None:
                out.append(None)
                continue
            aid = owner[st]
            arena, s, logical, e = active[aid]
            if addr < e:
                out.append(("extent", aid, arena, s, logical, e, addr - s))
            else:
                out.append(None)
        else:
            out.append(("rejected",))
    return out
