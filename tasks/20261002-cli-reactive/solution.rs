use std::cmp::Reverse;
use std::collections::BinaryHeap;
use std::io::{self, Read, Write};

fn main() {
    let mut inp = String::new();
    io::stdin().read_to_string(&mut inp).unwrap();
    let mut it = inp.split_ascii_whitespace();
    let mut nx = || it.next().unwrap();
    let n: usize = nx().parse().unwrap();
    let m: usize = nx().parse().unwrap();
    let q: usize = nx().parse().unwrap();
    let mut is_src = vec![false; n + 1];
    let mut cutoff = vec![0i64; n + 1];
    let mut base = vec![0i64; n + 1];
    for i in 1..=n {
        let t = nx();
        is_src[i] = t == "S";
        cutoff[i] = nx().parse().unwrap();
        base[i] = nx().parse().unwrap();
    }
    let mut eu = vec![0usize; m + 1];
    let mut ev = vec![0usize; m + 1];
    let mut ew = vec![0i64; m + 1];
    let mut outdeg = vec![0usize; n + 2];
    for e in 1..=m {
        eu[e] = nx().parse().unwrap();
        ev[e] = nx().parse().unwrap();
        ew[e] = nx().parse().unwrap();
        outdeg[eu[e]] += 1;
    }
    // CSR of outgoing edge ids
    let mut start = vec![0usize; n + 2];
    for i in 1..=n {
        start[i + 1] = start[i] + outdeg[i];
    }
    let mut fill = start.clone();
    let mut out = vec![0usize; m];
    for e in 1..=m {
        out[fill[eu[e]]] = e;
        fill[eu[e]] += 1;
    }
    // topo order
    let mut indeg = vec![0usize; n + 1];
    for e in 1..=m {
        indeg[ev[e]] += 1;
    }
    let mut order = Vec::with_capacity(n);
    for i in 1..=n {
        if indeg[i] == 0 {
            order.push(i);
        }
    }
    let mut h = 0;
    while h < order.len() {
        let u = order[h];
        h += 1;
        for &e in &out[start[u]..start[u + 1]] {
            let v = ev[e];
            indeg[v] -= 1;
            if indeg[v] == 0 {
                order.push(v);
            }
        }
    }
    let mut pos = vec![0usize; n + 1];
    for (i, &u) in order.iter().enumerate() {
        pos[u] = i;
    }
    // formula and shown values (wrapping arithmetic; true values fit i64)
    let mut formula = base.clone();
    let mut shown = vec![0i64; n + 1];
    for &u in &order {
        shown[u] = formula[u];
        for &e in &out[start[u]..start[u + 1]] {
            let v = ev[e];
            formula[v] = formula[v].wrapping_add(ew[e].wrapping_mul(shown[u]));
        }
    }

    let mut marked = vec![false; n + 1];
    let mut heap: BinaryHeap<Reverse<usize>> = BinaryHeap::new();
    let mut res = String::with_capacity(1 << 20);
    let mut props: Vec<(usize, i64)> = Vec::new();
    let mut rlist: Vec<usize> = Vec::new();
    let mut clist: Vec<usize> = Vec::new();
    for _ in 0..q {
        let k: usize = nx().parse().unwrap();
        let l: usize = nx().parse().unwrap();
        props.clear();
        for _ in 0..k {
            let s: usize = nx().parse().unwrap();
            let p: i64 = nx().parse().unwrap();
            props.push((s, p));
        }
        rlist.clear();
        clist.clear();
        // weight changes first, using pre-batch shown values
        for _ in 0..l {
            let e: usize = nx().parse().unwrap();
            let w: i64 = nx().parse().unwrap();
            if w != ew[e] {
                let v = ev[e];
                let d = w.wrapping_sub(ew[e]);
                formula[v] = formula[v].wrapping_add(d.wrapping_mul(shown[eu[e]]));
                ew[e] = w;
                if !marked[v] {
                    marked[v] = true;
                    heap.push(Reverse(pos[v]));
                }
            }
        }
        // source changes with new weights
        for &(s, p) in &props {
            let diff = (p as i128 - shown[s] as i128).abs();
            if diff > cutoff[s] as i128 {
                let d = p.wrapping_sub(shown[s]);
                shown[s] = p;
                clist.push(s);
                for &e in &out[start[s]..start[s + 1]] {
                    let v = ev[e];
                    formula[v] = formula[v].wrapping_add(ew[e].wrapping_mul(d));
                    if !marked[v] {
                        marked[v] = true;
                        heap.push(Reverse(pos[v]));
                    }
                }
            }
        }
        while let Some(Reverse(pi)) = heap.pop() {
            let u = order[pi];
            marked[u] = false;
            rlist.push(u);
            let f = formula[u];
            let diff = (f as i128 - shown[u] as i128).abs();
            if diff > cutoff[u] as i128 {
                let d = f.wrapping_sub(shown[u]);
                shown[u] = f;
                clist.push(u);
                for &e in &out[start[u]..start[u + 1]] {
                    let v = ev[e];
                    formula[v] = formula[v].wrapping_add(ew[e].wrapping_mul(d));
                    if !marked[v] {
                        marked[v] = true;
                        heap.push(Reverse(pos[v]));
                    }
                }
            }
        }
        rlist.sort_unstable();
        clist.sort_unstable();
        res.push_str("R ");
        res.push_str(&rlist.len().to_string());
        for &x in &rlist {
            res.push(' ');
            res.push_str(&x.to_string());
        }
        res.push_str(" C ");
        res.push_str(&clist.len().to_string());
        for &x in &clist {
            res.push(' ');
            res.push_str(&x.to_string());
        }
        res.push('\n');
    }
    io::stdout().write_all(res.as_bytes()).unwrap();
}
