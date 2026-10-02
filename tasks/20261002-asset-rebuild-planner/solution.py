import hashlib
import heapq


def _frame(s):
    b = s.encode("utf-8")
    return len(b).to_bytes(8, "big") + b


def _full_hash(local, pairs):
    # pairs: list of (name, hash), sorted by name
    parts = [b"ASSETv1\0", _frame(local), len(pairs).to_bytes(8, "big")]
    for name, h in pairs:
        parts.append(_frame(name))
        parts.append(_frame(h))
    return hashlib.sha256(b"".join(parts)).hexdigest()


def _recover(cache, log):
    begun = set()
    open_ops = {}  # op_id -> asset
    open_assets = set()
    invalid = False
    for rec in log:
        try:
            event, op, asset = rec
        except (TypeError, ValueError):
            invalid = True
            break
        if event == "begin":
            if op in begun or asset in open_assets:
                invalid = True
                break
            begun.add(op)
            open_ops[op] = asset
            open_assets.add(asset)
        elif event == "end":
            if op not in open_ops or open_ops[op] != asset:
                invalid = True
                break
            del open_ops[op]
            open_assets.discard(asset)
        else:
            invalid = True
            break
    if invalid:
        names = set(cache.keys())
        for rec in log:
            if isinstance(rec, (list, tuple)) and len(rec) >= 3:
                names.add(rec[2])
        return "invalid", sorted(names), None
    if open_ops:
        return "recoverable", sorted(open_assets), open_assets
    return "valid", [], set()


def _scc_cycles(names, deps):
    index = {}
    low = {}
    on_stack = set()
    stack = []
    counter = 0
    groups = []
    for root in names:
        if root in index:
            continue
        index[root] = low[root] = counter
        counter += 1
        stack.append(root)
        on_stack.add(root)
        work = [(root, iter(deps[root]))]
        while work:
            v, it = work[-1]
            advanced = False
            for w in it:
                if w not in index:
                    index[w] = low[w] = counter
                    counter += 1
                    stack.append(w)
                    on_stack.add(w)
                    work.append((w, iter(deps[w])))
                    advanced = True
                    break
                elif w in on_stack:
                    if index[w] < low[v]:
                        low[v] = index[w]
            if advanced:
                continue
            work.pop()
            if work:
                u = work[-1][0]
                if low[v] < low[u]:
                    low[u] = low[v]
            if low[v] == index[v]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                if len(comp) > 1 or v in deps[v]:
                    groups.append(sorted(comp))
    groups.sort()
    return groups


def plan_rebuild(local_hashes, dependencies, cache, transaction_log):
    recovery, discard, unusable = _recover(cache, transaction_log)
    names = sorted(local_hashes)
    deps = {n: list(dependencies.get(n, ())) for n in names}
    dep_sets = {n: set(d) for n, d in deps.items()}

    cycles = _scc_cycles(names, dep_sets)
    if cycles:
        return {"status": "cycles", "recovery": recovery, "discard": discard,
                "cycles": cycles}

    dependents = {n: [] for n in names}
    indeg = {}
    for n in names:
        indeg[n] = len(dep_sets[n])
        for d in dep_sets[n]:
            dependents[d].append(n)
    heap = [n for n in names if indeg[n] == 0]
    heapq.heapify(heap)
    full = {}
    rebuild = []
    while heap:
        n = heapq.heappop(heap)
        local = local_hashes[n]
        reused = False
        card = cache.get(n) if unusable is not None else None
        if card is not None and n not in unusable and isinstance(card, dict):
            try:
                if card.get("local") == local:
                    rec = card.get("dependencies") or []
                    rec_pairs = [(p[0], p[1]) for p in rec]
                    rec_names = [p[0] for p in rec_pairs]
                    if len(set(rec_names)) == len(rec_names) and set(rec_names) == dep_sets[n]:
                        rec_pairs.sort()
                        if all(full[d] == h for d, h in rec_pairs):
                            if _full_hash(card["local"], rec_pairs) == card.get("full"):
                                reused = True
            except (TypeError, KeyError, IndexError, AttributeError, ValueError):
                reused = False
        if reused:
            full[n] = card["full"]
        else:
            pairs = sorted((d, full[d]) for d in dep_sets[n])
            full[n] = _full_hash(local, pairs)
            rebuild.append(n)
        for m in dependents[n]:
            indeg[m] -= 1
            if indeg[m] == 0:
                heapq.heappush(heap, m)

    return {"status": "ok", "recovery": recovery, "discard": discard,
            "rebuild": rebuild,
            "full_hashes": [[n, full[n]] for n in names]}
