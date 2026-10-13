// Explicit codecs: no repr(C), pointer casts, or native structure serialization.
fn read(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}
fn write(b: &mut [u8], at: usize, value: u32) {
    b[at..at + 4].copy_from_slice(&value.to_le_bytes());
}
fn validate(b: &[u8]) -> u32 {
    let n = b.len();
    if n < 16 || n > u32::MAX as usize || read(b, 0) as usize != n { return 1; }
    if read(b, 4) != 1 { return 2; }
    let k = read(b, 8);
    if read(b, 12) != 0 || k > 6 { return 1; }
    if k == 6 {
        if n < 24 || read(b, 20) as usize != n - 24 { return 1; }
        return if read(b, 16) == 0 { 8 } else { 0 };
    }
    if n != (if k == 0 { 16 } else if k == 1 { 24 } else { 40 }) { return 1; }
    if (k == 2 || k == 3) && (read(b, 32) != 1 || read(b, 36) != 0) { return 1; }
    if k == 5 && (read(b, 28) > 255 || read(b, 32) > 255 || read(b, 36) > 255) { return 1; }
    0
}
fn main() {
    let payload: [[u32; 6]; 6] = [
        [1, 2, 0, 0, 0, 0], [1, 2, 10, 20, 1, 0], [7, 0, 1, 2, 1, 0],
        [7, 0, 1, 2, 11, 20], [1, 2, 3, 255, 128, 0], [1, 4, 42, 0, 0, 0],
    ];
    for k in 1..=6 {
        let n = if k == 1 { 24 } else if k == 6 { 28 } else { 40 };
        let mut b = vec![0; n];
        write(&mut b, 0, n as u32); write(&mut b, 4, 1); write(&mut b, 8, k);
        for at in (16..n).step_by(4) { write(&mut b, at, payload[k as usize - 1][(at - 16) / 4]); }
        assert_eq!(validate(&b), 0);
        for end in 0..n { assert_ne!(validate(&b[..end]), 0); }
        let mut bad = b.clone(); bad[12] = 1; assert_eq!(validate(&bad), 1);
        bad = b.clone(); write(&mut bad, 0, u32::MAX); assert_eq!(validate(&bad), 1);
        for byte in b { print!("{byte:02x}"); } println!();
    }
}
