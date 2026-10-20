//! Generates the frozen sixel input for the sixel-icy-overlay golden.
//!
//! Pattern (20x20 RGBA), five flat colours with overlapping layers:
//!   - background        #993322
//!   - green square 10x10 at (2,2)
//!   - blue square 10x10 at (8,8)   (overlaps green)
//!   - white 2-px cross, full extent (over everything)
//!
//! The overlap is what forces the encoder into the multi-layer regime:
//! each colour is a separate pass with a '$' reset, laid over the
//! previous one — the dialect shape ratatui sixel traffic takes.

use icy_sixel::SixelImage;
use std::io::Write;

fn main() -> Result<(), Box<std::io::Error>> {
	const W: usize = 20;
	const H: usize = 20;
	let bg: [u8; 4] = [0x99, 0x33, 0x22, 255];
	let green: [u8; 4] = [0x00, 0xaa, 0x00, 255];
	let blue: [u8; 4] = [0x00, 0x3c, 0xc8, 255];
	let white: [u8; 4] = [0xff, 0xff, 0xff, 255];

	let mut px = Vec::with_capacity(W * H * 4);
	for y in 0..H {
		for x in 0..W {
			let c = if x >= 2 && x < 12 && y >= 2 && y < 12 {
				green
			} else {
				bg
			};
			let c = if x >= 8 && x < 18 && y >= 8 && y < 18 {
				blue
			} else {
				c
			};
			let c = if x == 10 || x == 11 || y == 10 || y == 11 {
				white
			} else {
				c
			};
			px.extend_from_slice(&c);
		}
	}

	print!("{}", SixelImage::from_rgba(px, W, H).encode().unwrap());
	std::io::stdout().flush()?;
	Ok(())
}
