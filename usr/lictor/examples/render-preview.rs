//! Host-only visual fixture using the exact production renderer, never a
//! simulated authorization path. Run with --no-default-features on the host.
use lictor::{model::{Model, State}, render};
use std::{fs, io::Write, path::PathBuf};
fn main() {
    let out = PathBuf::from(std::env::args().nth(1).expect("output directory"));
    fs::create_dir_all(&out).unwrap();
    for (name, w, h, caps, state) in [
        ("post", 1280, 800, 1 << 13, State::Pending),
        ("dac-chown", 1280, 800, (1 << 7) | (1 << 8), State::Pending),
        ("all-minimum", 800, 720, 0x3f80, State::Pending),
        ("denied", 1280, 800, 1 << 9, State::Denied),
        ("empty", 1280, 800, 0, State::Empty),
    ] {
        let m = if caps == 0 { Model::default() } else { Model {
            state, pid: 2471, principal: 1000, stripes: 1, caps, term_ns: 0,
            request_deadline_ns: 1, propagating: true, user: b"michael".to_vec(),
            level: b"imperium".to_vec(), notice: Vec::new(),
        }};
        let mut pixels = vec![0; w * h];
        render::render(&mut pixels, w as u32, h as u32, None, &m, 0).unwrap();
        let mut file = fs::File::create(out.join(format!("{name}.ppm"))).unwrap();
        write!(file, "P6\n{w} {h}\n255\n").unwrap();
        let rgb: Vec<u8> = pixels.into_iter().flat_map(|p| [(p >> 16) as u8, (p >> 8) as u8, p as u8]).collect();
        file.write_all(&rgb).unwrap();
    }
}
