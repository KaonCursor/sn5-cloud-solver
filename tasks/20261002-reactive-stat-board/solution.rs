use std::collections::BinaryHeap;
use std::cmp::Reverse;
use std::io::{self, Read, Write};

fn main() {
    let mut inp = Vec::new();
    io::stdin().read_to_end(&mut inp).unwrap();
    let mut pos = 0usize;
    let mut tok = || -> &[u8] {
        while pos < inp.len() && inp[pos].is_ascii_whitespace() { pos += 1; }
        let s = pos;
        while pos < inp.len() && !inp[pos].is_ascii_whitespace() { pos += 1; }
        // safety: slice of inp lives as long as inp
        unsafe { std::slice::from_raw_parts(inp.as_ptr().add(s), pos - s) }
    };
    fn num(t: &[u8]) -> i64 {
        let (neg, d) = if t[0] == b'-' { (true, &t[1..]) } else if t[0] == b'+' { (false, &t[1..]) } else { (false, t) };
        let mut v: i64 = 0;
        for &c in d { v = v.wrapping_mul(10).wrapping_add((c - b'0') as i64); }
        if neg { v.wrapping_neg() } else { v }
    }
    let n = num(tok()) as usize;
    let m = num(tok()) as usize;
    let q = num(tok()) as usize;
    let mut is_src = vec![false; n + 1];
    let mut cutoff = vec![0i64; n + 1];
    let mut formula = vec![0i64; n + 1];
    let mut shown = vec![0i64; n + 1];
    for i in 1..=n {
        let t = tok();
        is_src[i] = t[0] == b'S';
        cutoff[i] = num(tok());
        let x = num(tok());
        if is_src[i] { shown[i] = x; } else { formula[i] = x; }
    }
    let mut eu = vec![0usize; m + 1];
    let mut ev = vec![0usize; m + 1];
    let mut ew = vec![0i64; m + 1];
    let mut deg = vec![0usize; n + 2];
    let mut indeg = vec![0usize; n + 1];
    for e in 1..=m {
        eu[e] = num(tok()) as usize;
        ev[e] = num(tok()) as usize;
        ew[e] = num(tok());
        deg[eu[e]] += 1;
        indeg[ev[e]] += 1;
    }
    // CSR outgoing
    let mut start = vec![0usize; n + 2];
    for i in 1..=n { start[i + 1] = start[i] + deg[i]; }
    let mut fill = start.clone();
    let mut adj = vec![0usize; m];
    for e in 1..=m { adj[fill[eu[e]]] = e; fill[eu[e]] += 1; }
    // topo order
    let mut topo = vec![0usize; n + 1];
    let mut order = Vec::with_capacity(n);
    for i in 1..=n { if indeg[i] == 0 { order.push(i); } }
    let mut h = 0;
    while h < order.len() {
        let u = order[h]; h += 1;
        for k in start[u]..start[u + 1] {
            let v = ev[adj[k]];
            indeg[v] -= 1;
            if indeg[v] == 0 { order.push(v); }
        }
    }
    for (idx, &u) in order.iter().enumerate() { topo[u] = idx; }
    // initial values
    for &u in &order {
        if !is_src[u] { shown[u] = formula[u]; }
        let su = shown[u];
        for k in start[u]..start[u + 1] {
            let e = adj[k];
            let v = ev[e];
            formula[v] = formula[v].wrapping_add(ew[e].wrapping_mul(su));
        }
    }
    let mut marked = vec![false; n + 1];
    let mut heap: BinaryHeap<Reverse<(usize, usize)>> = BinaryHeap::new();
    let mut out = String::with_capacity(1 << 20);
    let mut rl: Vec<usize> = Vec::new();
    let mut cl: Vec<usize> = Vec::new();
    let mut acc: Vec<(usize, i64)> = Vec::new();
    for _ in 0..q {
        let k = num(tok()) as usize;
        let l = num(tok()) as usize;
        rl.clear(); cl.clear(); acc.clear();
        for _ in 0..k {
            let s = num(tok()) as usize;
            let p = num(tok());
            let d = (p as i128 - shown[s] as i128).abs();
            if d > cutoff[s] as i128 { acc.push((s, p)); }
        }
        // weight changes using old shown values
        for _ in 0..l {
            let e = num(tok()) as usize;
            let nw = num(tok());
            if nw != ew[e] {
                let v = ev[e];
                formula[v] = formula[v].wrapping_add(nw.wrapping_sub(ew[e]).wrapping_mul(shown[eu[e]]));
                ew[e] = nw;
                if !marked[v] { marked[v] = true; heap.push(Reverse((topo[v], v))); }
            }
        }
        for &(s, p) in &acc {
            let delta = p.wrapping_sub(shown[s]);
            shown[s] = p;
            cl.push(s);
            for kk in start[s]..start[s + 1] {
                let e = adj[kk];
                let v = ev[e];
                formula[v] = formula[v].wrapping_add(ew[e].wrapping_mul(delta));
                if !marked[v] { marked[v] = true; heap.push(Reverse((topo[v], v))); }
            }
        }
        while let Some(Reverse((_, u))) = heap.pop() {
            marked[u] = false;
            rl.push(u);
            let f = formula[u];
            if (f as i128 - shown[u] as i128).abs() > cutoff[u] as i128 {
                let delta = f.wrapping_sub(shown[u]);
                shown[u] = f;
                cl.push(u);
                for kk in start[u]..start[u + 1] {
                    let e = adj[kk];
                    let v = ev[e];
                    formula[v] = formula[v].wrapping_add(ew[e].wrapping_mul(delta));
                    if !marked[v] { marked[v] = true; heap.push(Reverse((topo[v], v))); }
                }
            }
        }
        rl.sort_unstable(); cl.sort_unstable();
        use std::fmt::Write as W;
        write!(out, "R {}", rl.len()).unwrap();
        for x in &rl { write!(out, " {}", x).unwrap(); }
        write!(out, " C {}", cl.len()).unwrap();
        for x in &cl { write!(out, " {}", x).unwrap(); }
        out.push('\n');
    }
    io::stdout().write_all(out.as_bytes()).unwrap();
}
