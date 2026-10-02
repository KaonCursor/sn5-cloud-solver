from bisect import bisect_left, bisect_right
from operator import sub

MAX = 2**63 - 1
_B = 256


class _Extents:
    """Sorted active extents in buckets; each element stores the free gap before it."""

    def __init__(self):
        self.st = []   # per bucket: sorted starts
        self.en = []   # per bucket: ends
        self.gp = []   # per bucket: gap before each element
        self.mx = []   # per bucket: max gap
        self.first = []  # first start of each bucket

    def _rebuild(self, bi):
        if bi < 0 or bi >= len(self.st):
            return
        prev = self.en[bi - 1][-1] if bi > 0 else 0
        st, en = self.st[bi], self.en[bi]
        prevs = [prev]
        prevs += en[:-1]
        g = list(map(sub, st, prevs))
        self.gp[bi] = g
        self.mx[bi] = max(g)
        self.first[bi] = st[0]

    def _fix_head(self, bi):
        """Recompute only the first gap of bucket bi (its predecessor changed)."""
        if bi <= 0 or bi >= len(self.st):
            if bi == 0 and self.st:
                self._rebuild(0)
            return
        g = self.gp[bi]
        g[0] = self.st[bi][0] - self.en[bi - 1][-1]
        self.mx[bi] = max(g)

    def _locate(self, x):
        """Index of bucket that would contain start x (last bucket with first <= x)."""
        bi = bisect_right(self.first, x) - 1
        return bi if bi >= 0 else 0

    def containing(self, addr):
        if not self.st:
            return None
        bi = bisect_right(self.first, addr) - 1
        if bi < 0:
            return None
        st = self.st[bi]
        i = bisect_right(st, addr) - 1
        if i >= 0 and self.en[bi][i] > addr:
            return st[i]
        return None

    def free(self, s, e):
        if not self.st:
            return True
        bi = bisect_right(self.first, s) - 1
        if bi >= 0:
            st = self.st[bi]
            i = bisect_right(st, s) - 1
            if self.en[bi][i] > s:
                return False
            # successor
            if i + 1 < len(st):
                return st[i + 1] >= e
            if bi + 1 < len(self.st):
                return self.first[bi + 1] >= e
            return True
        return self.first[0] >= e

    def next_start(self, s):
        """Smallest active start strictly greater than s, or None."""
        bi = bisect_right(self.first, s) - 1
        if bi < 0:
            return self.first[0] if self.first else None
        st = self.st[bi]
        i = bisect_right(st, s)
        if i < len(st):
            return st[i]
        if bi + 1 < len(self.st):
            return self.first[bi + 1]
        return None

    def insert(self, s, e):
        if not self.st:
            self.st.append([s]); self.en.append([e])
            self.gp.append([s]); self.mx.append(s); self.first.append(s)
            return
        bi = self._locate(s)
        st = self.st[bi]
        i = bisect_left(st, s)
        st.insert(i, s)
        self.en[bi].insert(i, e)
        if len(st) > 2 * _B:
            h = len(st) // 2
            self.st[bi:bi + 1] = [st[:h], st[h:]]
            en = self.en[bi]
            self.en[bi:bi + 1] = [en[:h], en[h:]]
            self.gp[bi:bi + 1] = [[], []]
            self.mx[bi:bi + 1] = [0, 0]
            self.first[bi:bi + 1] = [0, 0]
            self._rebuild(bi)
            self._rebuild(bi + 1)
            self._fix_head(bi + 2)
        else:
            en = self.en[bi]
            prev = en[i - 1] if i > 0 else (self.en[bi - 1][-1] if bi > 0 else 0)
            g = self.gp[bi]
            g.insert(i, s - prev)
            if i + 1 < len(st):
                g[i + 1] = st[i + 1] - e
            else:
                self._fix_head(bi + 1)
            self.mx[bi] = max(g)
            self.first[bi] = st[0]

    def remove(self, s):
        bi = bisect_right(self.first, s) - 1
        st = self.st[bi]
        i = bisect_left(st, s)
        del st[i]
        del self.en[bi][i]
        if not st:
            del self.st[bi], self.en[bi], self.gp[bi], self.mx[bi], self.first[bi]
            self._fix_head(bi)
        else:
            en = self.en[bi]
            g = self.gp[bi]
            del g[i]
            if i < len(st):
                prev = en[i - 1] if i > 0 else (self.en[bi - 1][-1] if bi > 0 else 0)
                g[i] = st[i] - prev
            else:
                self._fix_head(bi + 1)
            self.mx[bi] = max(g)
            self.first[bi] = st[0]

    def lowest_fit(self, size, limit):
        """Lowest start (aligned since all ends are aligned) with a free gap of size."""
        for bi in range(len(self.st)):
            if self.mx[bi] >= size:
                g = self.gp[bi]
                for i in range(len(g)):
                    if g[i] >= size:
                        cand = self.en[bi][i - 1] if i > 0 else (self.en[bi - 1][-1] if bi > 0 else 0)
                        return cand if cand + size <= limit else None
        last = self.en[-1][-1] if self.en else 0
        return last if last + size <= limit else None


def extent_journal(chunk_size, address_limit, large_threshold, events):
    def phys(size):
        if size < large_threshold or size > MAX or size + chunk_size - 1 > MAX:
            return None
        return (size + chunk_size - 1) // chunk_size * chunk_size

    ex = _Extents()
    allocs = {}      # id -> [arena, start, logical, end]
    by_start = {}    # start -> id
    out = []
    for ev in events:
        op = ev[0]
        if op == "claim":
            _, aid, arena, start, size = ev
            p = phys(size)
            if (aid in allocs or p is None or start < 0 or start % chunk_size
                    or start + p > address_limit or not ex.free(start, start + p)):
                out.append(("rejected",))
                continue
            ex.insert(start, start + p)
            allocs[aid] = [arena, start, size, start + p]
            by_start[start] = aid
            out.append(("claimed", start, start + p))
        elif op == "resize":
            _, aid, size = ev
            a = allocs.get(aid)
            p = phys(size)
            if a is None or p is None:
                out.append(("rejected",))
                continue
            arena, os_, ol, oe = a
            ne = os_ + p
            in_place = False
            if p <= oe - os_:
                in_place = True
            elif ne <= address_limit:
                nx = ex.next_start(os_)
                if nx is None or ne <= nx:
                    in_place = True
            if in_place:
                ex.remove(os_)
                ex.insert(os_, ne)
                a[2] = size
                a[3] = ne
                out.append(("resized", os_, oe, os_, ne))
                continue
            ns = ex.lowest_fit(p, address_limit)
            if ns is None:
                out.append(("rejected",))
                continue
            ex.remove(os_)
            del by_start[os_]
            ex.insert(ns, ns + p)
            by_start[ns] = aid
            a[1] = ns; a[2] = size; a[3] = ns + p
            out.append(("resized", os_, oe, ns, ns + p))
        elif op == "release":
            aid = ev[1]
            a = allocs.pop(aid, None)
            if a is None:
                out.append(("rejected",))
                continue
            ex.remove(a[1])
            del by_start[a[1]]
            out.append(("released", a[1], a[3]))
        elif op == "lookup":
            addr = ev[1]
            s = ex.containing(addr)
            if s is None:
                out.append(None)
            else:
                aid = by_start[s]
                arena, st, lg, en = allocs[aid]
                out.append(("extent", aid, arena, st, lg, en, addr - st))
    return out
