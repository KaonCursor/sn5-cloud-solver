use std::collections::HashMap;
use std::io::{self, Read, Write};

struct Key {
    fw: Vec<u128>,
    exp: Vec<u64>,
    head: usize, // 1-indexed first possibly active entry
}

impl Key {
    fn new() -> Self {
        Key { fw: vec![0], exp: vec![0], head: 1 }
    }
    fn len(&self) -> usize {
        self.fw.len() - 1
    }
    fn prefix(&self, mut i: usize) -> u128 {
        let mut s = 0u128;
        while i > 0 {
            s += self.fw[i];
            i &= i - 1;
        }
        s
    }
    fn push(&mut self, val: u128, exp: u64) -> usize {
        let i = self.len() + 1;
        let low = i & i.wrapping_neg();
        let v = val + self.prefix(i - 1) - self.prefix(i - low);
        self.fw.push(v);
        self.exp.push(exp);
        i
    }
    fn sub(&mut self, mut i: usize, val: u128) {
        let n = self.len();
        while i <= n {
            self.fw[i] -= val;
            i += i & i.wrapping_neg();
        }
    }
    fn expire(&mut self, t: u64) {
        let n = self.len();
        while self.head <= n && self.exp[self.head] <= t {
            self.head += 1;
        }
    }
    fn active(&self) -> u128 {
        self.prefix(self.len()) - self.prefix(self.head - 1)
    }
    // smallest k with prefix(k) >= target
    fn find(&self, target: u128) -> usize {
        let n = self.len();
        let mut step = 1usize;
        while step * 2 <= n {
            step *= 2;
        }
        let mut pos = 0usize;
        let mut rem = target;
        while step > 0 {
            if pos + step <= n && self.fw[pos + step] < rem {
                pos += step;
                rem -= self.fw[pos];
            }
            step >>= 1;
        }
        pos + 1
    }
    // earliest time >= t when active + cost <= b (assumes expire(t) done)
    fn ready(&self, t: u64, cost: u128, b: u128) -> u64 {
        let a = self.active();
        if a + cost <= b {
            return t;
        }
        let need = a + cost - b;
        let k = self.find(self.prefix(self.head - 1) + need);
        self.exp[k].max(t)
    }
}

fn main() {
    let mut input = Vec::new();
    io::stdin().read_to_end(&mut input).unwrap();
    let mut it = input.split(|c| c.is_ascii_whitespace()).filter(|s| !s.is_empty());
    let num = |s: &[u8]| -> u64 { s.iter().fold(0u64, |a, &c| a * 10 + (c - b'0') as u64) };
    let n = num(it.next().unwrap()) as usize;
    let wi = num(it.next().unwrap());
    let bi = num(it.next().unwrap()) as u128;
    let we = num(it.next().unwrap());
    let be = num(it.next().unwrap()) as u128;

    let mut ip_map: HashMap<&[u8], usize> = HashMap::new();
    let mut em_map: HashMap<Vec<u8>, usize> = HashMap::new();
    let mut ips: Vec<Key> = Vec::new();
    let mut ems: Vec<Key> = Vec::new();
    // per request id: Some((ipk, ipi, emk, emi, cost, revoked))
    let mut reqs: Vec<Option<(usize, usize, usize, usize, u128, bool)>> = Vec::new();
    let mut out = String::with_capacity(n * 10);

    for _ in 0..n {
        let kind = it.next().unwrap();
        let t = num(it.next().unwrap());
        if kind[0] == b'R' {
            let ip = it.next().unwrap();
            let em = it.next().unwrap();
            let cost = num(it.next().unwrap()) as u128;
            let proof = it.next().unwrap();
            if proof == b"0" {
                out.push_str("CAPTCHA\n");
                reqs.push(None);
                continue;
            }
            if cost > bi || cost > be {
                out.push_str("NEVER\n");
                reqs.push(None);
                continue;
            }
            let ipk = *ip_map.entry(ip).or_insert_with(|| {
                ips.push(Key::new());
                ips.len() - 1
            });
            let eml = em.to_ascii_lowercase();
            let emk = match em_map.get(&eml) {
                Some(&k) => k,
                None => {
                    ems.push(Key::new());
                    em_map.insert(eml, ems.len() - 1);
                    ems.len() - 1
                }
            };
            ips[ipk].expire(t);
            ems[emk].expire(t);
            let zi = ips[ipk].ready(t, cost, bi);
            let ze = ems[emk].ready(t, cost, be);
            if zi == t && ze == t {
                let ipi = ips[ipk].push(cost, t + wi);
                let emi = ems[emk].push(cost, t + we);
                out.push_str("ACCEPT\n");
                reqs.push(Some((ipk, ipi, emk, emi, cost, false)));
            } else {
                out.push_str("RETRY ");
                out.push_str(&zi.max(ze).to_string());
                out.push('\n');
                reqs.push(None);
            }
        } else {
            let id = it.next().unwrap().iter().fold(0usize, |a, &c| a.saturating_mul(10).saturating_add((c - b'0') as usize));
            let mut ok = false;
            if id >= 1 && id <= reqs.len() {
                if let Some(r) = reqs[id - 1].as_mut() {
                    if !r.5 {
                        r.5 = true;
                        ok = true;
                        ips[r.0].sub(r.1, r.4);
                        ems[r.2].sub(r.3, r.4);
                    }
                }
            }
            out.push_str(if ok { "REVOKED\n" } else { "INVALID\n" });
        }
    }
    io::stdout().write_all(out.as_bytes()).unwrap();
}
