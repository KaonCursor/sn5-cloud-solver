import hashlib
import heapq


def plan_rebuild(local_hashes, dependencies, cache, transaction_log):
    def frame(s):
        b = s.encode("utf-8")
        return len(b).to_bytes(8, "big") + b

    def full_hash(local, pairs):
        parts = [b"ASSETv1\0", frame(local), len(pairs).to_bytes(8, "big")]
        for n, h in sorted(pairs, key=lambda p: p[0]):
            parts.append(frame(n))
            parts.append(frame(h))
        return hashlib.sha256(b"".join(parts)).hexdigest()

    # recovery
    invalid = False
    begun = set()
    open_ops = {}
    open_assets = set()
    log_assets = set()
    for rec in transaction_log:
        ev, op, name = rec[0], rec[1], rec[2]
        log_assets.add(name)
        if ev == "begin":
            if op in begun or name in open_assets:
                invalid = True
                break
            begun.add(op)
            open_ops[op] = name
            open_assets.add(name)
        elif ev == "end":
            if op not in open_ops or open_ops[op] != name:
                invalid = True
                break
            del open_ops[op]
            open_assets.discard(name)
        else:
            invalid = True
            break
    if invalid:
        recovery = "invalid"
        # log_assets may be partial if we broke early; collect all
        for rec in transaction_log:
            log_assets.add(rec[2])
        discard = sorted(set(cache) | log_assets)
        discarded = set(discard)
    elif open_ops:
        recovery = "recoverable"
        discarded = set(open_ops.values())
        discard = sorted(discarded)
    else:
        recovery = "valid"
        discard = []
        discarded = set()

    # SCC (iterative Tarjan)
    names = list(local_hashes)
    index = {}
    low = {}
    onstack = set()
    stack = []
    cycles = []
    counter = 0
    for root in names:
        if root in index:
            continue
        index[root] = low[root] = counter
        counter += 1
        stack.append(root)
        onstack.add(root)
        work = [(root, iter(dependencies[root]))]
        while work:
            v, it = work[-1]
            adv = False
            for w in it:
                if w not in index:
                    index[w] = low[w] = counter
                    counter += 1
                    stack.append(w)
                    onstack.add(w)
                    work.append((w, iter(dependencies[w])))
                    adv = True
                    break
                elif w in onstack:
                    if index[w] < low[v]:
                        low[v] = index[w]
            if adv:
                continue
            work.pop()
            if work:
                p = work[-1][0]
                if low[v] < low[p]:
                    low[p] = low[v]
            if low[v] == index[v]:
                comp = []
                while True:
                    w = stack.pop()
                    onstack.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                if len(comp) > 1 or v in dependencies[v]:
                    cycles.append(sorted(comp))
    if cycles:
        cycles.sort()
        return {"status": "cycles", "recovery": recovery,
                "discard": discard, "cycles": cycles}

    # topological order with smallest-name choice
    rdeps = {n: [] for n in names}
    indeg = {}
    for n in names:
        indeg[n] = len(dependencies[n])
        for d in dependencies[n]:
            rdeps[d].append(n)
    heap = [n for n in names if indeg[n] == 0]
    heapq.heapify(heap)
    full = {}
    rebuild = []
    while heap:
        n = heapq.heappop(heap)
        deps = dependencies[n]
        cur = full_hash(local_hashes[n], [(d, full[d]) for d in deps])
        reuse = False
        card = cache.get(n)
        if card is not None and n not in discarded and recovery != "invalid":
            pairs = card["dependencies"]
            if (card["local"] == local_hashes[n]
                    and len(pairs) == len(deps)
                    and {p[0] for p in pairs} == set(deps)
                    and card["full"] == full_hash(card["local"], [(p[0], p[1]) for p in pairs])
                    and all(full[p[0]] == p[1] for p in pairs)):
                reuse = True
        if not reuse:
            rebuild.append(n)
        full[n] = cur
        for m in rdeps[n]:
            indeg[m] -= 1
            if indeg[m] == 0:
                heapq.heappush(heap, m)
    return {"status": "ok", "recovery": recovery, "discard": discard,
            "rebuild": rebuild,
            "full_hashes": [[n, full[n]] for n in sorted(full)]}
