use std::collections::BTreeMap;
use std::io::{Read, Write};

fn add_point(iv: &mut BTreeMap<u64, u64>, k: u64) {
    let mut s = k;
    let mut e = k;
    if let Some((&ps, &pe)) = iv.range(..=k).next_back() {
        if pe >= k { return; }
        if pe + 1 == k { s = ps; iv.remove(&ps); }
    }
    if let Some(&ne) = iv.get(&(k + 1)) {
        e = ne;
        iv.remove(&(k + 1));
    }
    iv.insert(s, e);
}

fn remove_range(iv: &mut BTreeMap<u64, u64>, l: u64, r: u64) {
    let mut hits = Vec::new();
    if let Some((&ps, &pe)) = iv.range(..l).next_back() {
        if pe >= l { hits.push((ps, pe)); }
    }
    for (&s, &e) in iv.range(l..=r) { hits.push((s, e)); }
    for (s, e) in hits {
        iv.remove(&s);
        if s < l { iv.insert(s, l - 1); }
        if e > r { iv.insert(r + 1, e); }
    }
}

fn main() {
    let mut inp = String::new();
    std::io::stdin().read_to_string(&mut inp).unwrap();
    let mut t = inp.split_ascii_whitespace();
    let n: u64 = t.next().unwrap().parse().unwrap();
    let q: usize = t.next().unwrap().parse().unwrap();
    let m: usize = t.next().unwrap().parse().unwrap();
    let mut off: u64 = 0;
    let mut cur: u64 = 0;
    let mut items: BTreeMap<u64, usize> = BTreeMap::new(); // key -> age id
    let mut ages: Vec<(u64, i64, bool)> = Vec::new();
    let mut iv: BTreeMap<u64, u64> = BTreeMap::new();
    for _ in 0..m {
        let i: u64 = t.next().unwrap().parse().unwrap();
        let v: i64 = t.next().unwrap().parse().unwrap();
        items.insert(i, ages.len());
        ages.push((i, v, true));
        add_point(&mut iv, i);
    }
    let mut out = String::new();
    let key = |a: u64, off: u64| (a + n - off) % n;
    for _ in 0..q {
        let op = t.next().unwrap();
        match op {
            "MOVE" => { cur = t.next().unwrap().parse().unwrap(); }
            "SHIFT" => {
                let d: u64 = t.next().unwrap().parse().unwrap();
                off = (off + d) % n;
                cur = (cur + d) % n;
            }
            "INSERT" => {
                let x: i64 = t.next().unwrap().parse().unwrap();
                if items.len() as u64 == n { out.push_str("-1\n"); continue; }
                let mut p = key(cur, off);
                loop {
                    match iv.range(..=p).next_back() {
                        Some((_, &e)) if e >= p => { p = (e + 1) % n; }
                        _ => break,
                    }
                }
                items.insert(p, ages.len());
                ages.push((p, x, true));
                add_point(&mut iv, p);
                out.push_str(&format!("{}\n", (p + off) % n));
                cur = (cur + (p + n - key(cur, off)) % n + 1) % n;
            }
            "RING" => {
                let x: i64 = t.next().unwrap().parse().unwrap();
                let k = key(cur, off);
                if let Some(id) = items.remove(&k) { ages[id].2 = false; } else { add_point(&mut iv, k); }
                items.insert(k, ages.len());
                ages.push((k, x, true));
                out.push_str(&format!("{}\n", cur));
                cur = (cur + 1) % n;
            }
            _ => {
                let a: u64 = t.next().unwrap().parse().unwrap();
                let b: u64 = t.next().unwrap().parse().unwrap();
                let ka = key(a, off);
                let kb = key(b, off);
                let mut ranges = Vec::new();
                // actual range a..b circular -> key range ka..kb circular
                let span = (b + n - a) % n; // length-1
                let _ = span;
                if ka <= kb { ranges.push((ka, kb)); } else { ranges.push((ka, n - 1)); ranges.push((0, kb)); }
                for (l, r) in ranges {
                    let ks: Vec<u64> = items.range(l..=r).map(|(&k, _)| k).collect();
                    for k in ks { let id = items.remove(&k).unwrap(); ages[id].2 = false; }
                    remove_range(&mut iv, l, r);
                }
            }
        }
    }
    out.push_str(&format!("{}\n", items.len()));
    for &(k, v, alive) in &ages {
        if alive { out.push_str(&format!("{} {}\n", (k + off) % n, v)); }
    }
    std::io::stdout().write_all(out.as_bytes()).unwrap();
}
