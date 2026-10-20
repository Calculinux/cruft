//! One-shot ratatui-image driver for the cruft L2b pixel golden
//! (kitty-applive). This is the *peer* side of the live interop: a real
//! app talking the real protocol to the product under test.
//!
//! Determinism contract (why the pty trace byte-freezes across hosts):
//!   - single draw, no event loop, no timestamps, no user input;
//!   - Picker negotiation happens here, before any stdin event reads,
//!     and the product under test answers it;
//!   - SHM and compression probes are off by default in the picker, so
//!     `rand` (SHM names) and `flate2` (o=z) stay out of the traffic;
//!   - transfer is fire-and-forget in ratatui-image: it never waits on
//!     the product's transfer replies, so no timing race enters the
//!     stream;
//!   - the image scaler is a fixed filter over a committed PNG.

use std::error::Error;

use image::DynamicImage;
use ratatui::{
	Terminal,
	backend::CrosstermBackend,
	layout::{Rect, Size},
	widgets::Paragraph,
};
use ratatui_image::{Image, Resize, picker::Picker};

fn main() -> Result<(), Box<dyn Error>> {
	let path = std::env::args().nth(1).expect("usage: apptest-asset <png>");
	let img = DynamicImage::ImageRgba8(image::open(&path)?.to_rgba8());

	let backend = CrosstermBackend::new(std::io::stdout());
	let mut term = Terminal::new(backend)?;
	term.clear()?;

	// Capability negotiation against the product under test. Must run
	// before anything else touches stdin (see Picker docs).
	let picker = Picker::from_query_stdio()?;

	let rect = Rect::new(6, 4, 40, 20);
	let proto = picker.new_protocol(img, Size::new(40, 20), Resize::Fit(None))?;
	let img_widget = Image::new(&proto);

	term.draw(|f| {
		f.render_widget(
			Paragraph::new("L2B-LIVE-KGP"),
			Rect::new(6, 2, 20, 1),
		);
		f.render_widget(img_widget, rect);
		f.render_widget(
			Paragraph::new("FOOTER"),
			Rect::new(6, 24, 10, 1),
		);
	})?;

	// Normal exit: stdout flushes, the pty delivers EOF to the driver,
	// and every chunk the app staged is already queued on the wire.
	Ok(())
}
