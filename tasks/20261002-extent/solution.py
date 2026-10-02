from bisect import bisect_left, bisect_right


def extent_journal(chunk_size, address_limit, large_threshold, events):
    MAX = 2**63 - 1
    c = chunk_size

    def conv(size):
        if size < large_threshold or size > MAX or size + c - 1 > MAX:
            return None
        return (size + c - 1) // c * c

    starts = []
    info = {}  # start -> [id, arena, logical, end]
    active = {}  # id -> start
    out = []
    for ev in events:
        op = ev[0]
        if op == "claim":
            _, aid, arena, start, size = ev
            p = None
            if aid not in active and start >= 0 and start % c == 0:
                p = conv(size)
            if p is None or start + p > address_limit:
                out.append(("rejected",))
                continue
            end = start + p
            i = bisect_left(starts, start)
            if i > 0 and info[starts[i - 1]][3] > start:
                out.append(("rejected",))
                continue
            if i < len(starts) and starts[i] < end:
                out.append(("rejected",))
                continue
            starts.insert(i, start)
            info[start] = [aid, arena, size, end]
            active[aid] = start
            out.append(("claimed", start, end))
        elif op == "resize":
            _, aid, size = ev
            if aid not in active:
                out.append(("rejected",))
                continue
            p = conv(size)
            if p is None:
                out.append(("rejected",))
                continue
            s = active[aid]
            rec = info[s]
            old_end = rec[3]
            if p <= old_end - s:
                rec[2] = size
                rec[3] = s + p
                out.append(("resized", s, old_end, s, s + p))
                continue
            i = bisect_left(starts, s)
            nxt = starts[i + 1] if i + 1 < len(starts) else address_limit
            ne = s + p
            if ne <= address_limit and ne <= nxt:
                rec[2] = size
                rec[3] = ne
                out.append(("resized", s, old_end, s, ne))
                continue
            pos = 0
            found = None
            for st in starts:
                if st - pos >= p:
                    found = pos
                    break
                pos = info[st][3]
            if found is None and address_limit - pos >= p:
                found = pos
            if found is None:
                out.append(("rejected",))
                continue
            del starts[i]
            del info[s]
            j = bisect_left(starts, found)
            starts.insert(j, found)
            info[found] = [aid, rec[1], size, found + p]
            active[aid] = found
            out.append(("resized", s, old_end, found, found + p))
        elif op == "release":
            aid = ev[1]
            if aid not in active:
                out.append(("rejected",))
                continue
            s = active.pop(aid)
            rec = info.pop(s)
            del starts[bisect_left(starts, s)]
            out.append(("released", s, rec[3]))
        else:
            a = ev[1]
            i = bisect_right(starts, a) - 1
            if i < 0:
                out.append(None)
                continue
            s = starts[i]
            rec = info[s]
            if a >= rec[3]:
                out.append(None)
            else:
                out.append(("extent", rec[0], rec[1], s, rec[2], rec[3], a - s))
    return out
