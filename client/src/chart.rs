//! AAS stacked bar chart widget.
//!
//! Two rendering paths:
//! - **Pixel**: true pixel rendering via ratatui-image (iTerm2/Sixel/Kitty)
//! - **Half-block** (fallback): '▄' technique for 2× vertical resolution

use ratatui::prelude::*;
use image::{RgbImage, Rgb};
use crate::compute::{AasBucketResult, NUM_WAIT_CLASSES};

// -- Color palette (ASH-style) ------------------------------------------------

const CLASS_COLORS: [Color; NUM_WAIT_CLASSES] = [
    Color::Rgb(80, 250, 123),   // 0 Cpu: green
    Color::Rgb(30, 100, 255),   // 1 Io: vivid blue
    Color::Rgb(255, 85, 85),    // 2 Lock: red
    Color::Rgb(255, 121, 198),  // 3 LwLock: pink
    Color::Rgb(0, 200, 255),    // 4 Ipc: cyan
    Color::Rgb(255, 220, 100),  // 5 Client: yellow
    Color::Rgb(255, 165, 0),    // 6 Timeout: orange
    Color::Rgb(0, 210, 180),    // 7 BufferPin: teal
    Color::Rgb(150, 100, 255),  // 8 Activity: purple
    Color::Rgb(190, 150, 255),  // 9 Extension: light purple
    Color::Rgb(180, 180, 180),  // 10 Unknown: gray
];

const CLASS_LABELS: [&str; NUM_WAIT_CLASSES] = [
    "CPU", "IO", "Lock", "LwLock", "IPC", "Client", "Timeout",
    "BufferPin", "Activity", "Extension", "Unknown",
];

const CLASS_COLORS_RGB: [[u8; 3]; NUM_WAIT_CLASSES] = [
    [80, 250, 123],   // 0 Cpu: green
    [30, 100, 255],   // 1 Io: vivid blue
    [255, 85, 85],    // 2 Lock: red
    [255, 121, 198],  // 3 LwLock: pink
    [0, 200, 255],    // 4 Ipc: cyan
    [255, 220, 100],  // 5 Client: yellow
    [255, 165, 0],    // 6 Timeout: orange
    [0, 210, 180],    // 7 BufferPin: teal
    [150, 100, 255],  // 8 Activity: purple
    [190, 150, 255],  // 9 Extension: light purple
    [180, 180, 180],  // 10 Unknown: gray
];

const Y_AXIS_W: u16 = 6; // 5 chars for label + 1 for '│'

// -- Pixel chart rendering ----------------------------------------------------

/// Return the sub-rect for the chart body (excluding y-axis, x-axis, legend).
pub fn chart_body_rect(area: Rect) -> Rect {
    if area.width < 12 || area.height < 5 {
        return Rect::default();
    }
    Rect {
        x: area.x + Y_AXIS_W,
        y: area.y,
        width: area.width.saturating_sub(Y_AXIS_W + 1),
        height: area.height.saturating_sub(2),
    }
}

/// Render the stacked bar chart body as a pixel image.
pub fn chart_body_image(result: &AasBucketResult, width: u32, height: u32) -> RgbImage {
    let mut img = RgbImage::from_pixel(width, height, Rgb([0, 0, 0]));
    let nbuckets = result.buckets.len();
    if nbuckets == 0 || height == 0 || width == 0 {
        return img;
    }

    let max_aas = result.max_aas.max(0.01);
    let col_w = width as f64 / nbuckets as f64;

    for (i, bucket) in result.buckets.iter().enumerate() {
        let x0 = (i as f64 * col_w) as u32;
        let x1 = (((i + 1) as f64) * col_w) as u32;

        let mut cum = 0.0f64;
        for class_idx in 0..NUM_WAIT_CLASSES {
            let aas = bucket.class_aas[class_idx];
            if aas <= 0.0 { continue; }

            let y_bottom = height - ((cum / max_aas) * height as f64).min(height as f64) as u32;
            cum += aas;
            let y_top = height - ((cum / max_aas) * height as f64).min(height as f64) as u32;

            let color = Rgb(CLASS_COLORS_RGB[class_idx]);
            for x in x0..x1.min(width) {
                for y in y_top..y_bottom.min(height) {
                    img.put_pixel(x, y, color);
                }
            }
        }
    }

    img
}

/// Render chart text decorations (y-axis, x-axis labels, legend) — used with pixel path.
pub fn render_chart_decorations(
    result: &AasBucketResult,
    from_ns: u64,
    to_ns: u64,
    area: Rect,
    buf: &mut Buffer,
) {
    if area.width < 12 || area.height < 5 { return; }

    let body_height = area.height.saturating_sub(2) as usize;
    let body_width = area.width.saturating_sub(Y_AXIS_W + 1) as usize;
    let body_x = area.x + Y_AXIS_W;
    let xaxis_y = area.y + body_height as u16;
    let legend_y = xaxis_y + 1;

    let max_aas = if result.max_aas > 0.0 { result.max_aas } else { 1.0 };

    // Y-axis separator
    for row in 0..body_height {
        set_cell(buf, area.x + Y_AXIS_W - 1, area.y + row as u16,
                 '│', Style::new().fg(Color::DarkGray));
    }

    // Y-axis labels
    render_y_label(buf, area.x, area.y, max_aas);
    if body_height > 2 {
        render_y_label(buf, area.x, area.y + (body_height / 2) as u16, max_aas / 2.0);
    }
    render_y_label(buf, area.x, area.y + (body_height - 1) as u16, 0.0);

    // X-axis
    render_x_axis(buf, body_x, xaxis_y, body_width, from_ns, to_ns);

    // Legend
    render_legend(buf, area.x, legend_y, area.width as usize, result);
}

// -- Half-block class color lookup --------------------------------------------

/// Given a virtual row index within a column's stack, return the color of the
/// wait class occupying that row, or None if the row is empty.
fn class_color_at_vrow(
    class_aas: &[f64; NUM_WAIT_CLASSES],
    vrow: usize,
    total_vrows: usize,
    max_aas: f64,
) -> Option<Color> {
    let mut cum = 0.0f64;
    for idx in 0..NUM_WAIT_CLASSES {
        let aas = class_aas[idx];
        if aas <= 0.0 {
            continue;
        }
        let vrow_start = (cum / max_aas * total_vrows as f64) as usize;
        cum += aas;
        let vrow_end = (cum / max_aas * total_vrows as f64).ceil() as usize;
        if vrow >= vrow_start && vrow < vrow_end {
            return Some(CLASS_COLORS[idx]);
        }
    }
    None
}

// -- Widget -------------------------------------------------------------------

pub struct AasChart<'a> {
    result: &'a AasBucketResult,
    from_ns: u64,
    to_ns: u64,
}

impl<'a> AasChart<'a> {
    pub fn new(result: &'a AasBucketResult, from_ns: u64, to_ns: u64) -> Self {
        Self { result, from_ns, to_ns }
    }
}

impl Widget for AasChart<'_> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        if area.width < 12 || area.height < 5 {
            return;
        }

        let body_x = area.x + Y_AXIS_W;
        let body_width = area.width.saturating_sub(Y_AXIS_W + 1) as usize;
        let body_height = area.height.saturating_sub(2) as usize; // -1 x-axis, -1 legend
        let body_y = area.y;
        let xaxis_y = area.y + body_height as u16;
        let legend_y = xaxis_y + 1;

        if body_width == 0 || body_height == 0 {
            return;
        }

        let max_aas = if self.result.max_aas > 0.0 {
            self.result.max_aas
        } else {
            1.0
        };
        let total_vrows = body_height * 2;

        // -- Y-axis separator line --
        for row in 0..body_height {
            set_cell(buf, area.x + Y_AXIS_W - 1, body_y + row as u16,
                     '│', Style::new().fg(Color::DarkGray));
        }

        // -- Y-axis labels (top, middle, bottom) --
        render_y_label(buf, area.x, body_y, max_aas);
        if body_height > 2 {
            render_y_label(buf, area.x, body_y + (body_height / 2) as u16, max_aas / 2.0);
        }
        render_y_label(buf, area.x, body_y + (body_height - 1) as u16, 0.0);

        // -- Chart body (half-block rendering) --
        let nbuckets = self.result.buckets.len().min(body_width);
        for col in 0..nbuckets {
            let bucket = &self.result.buckets[col];
            for row in 0..body_height {
                // Map terminal row to virtual rows (bottom-up)
                let bottom_vrow = (body_height - 1 - row) * 2;
                let top_vrow = bottom_vrow + 1;

                let bc = class_color_at_vrow(&bucket.class_aas, bottom_vrow, total_vrows, max_aas);
                let tc = class_color_at_vrow(&bucket.class_aas, top_vrow, total_vrows, max_aas);

                let x = body_x + col as u16;
                let y = body_y + row as u16;

                match (bc, tc) {
                    (None, None) => {} // empty — leave default
                    (Some(c1), Some(c2)) if c1 == c2 => {
                        set_cell(buf, x, y, '█', Style::new().fg(c1));
                    }
                    (Some(b), Some(t)) => {
                        // Lower half = bottom color (fg), upper half = top color (bg)
                        set_cell(buf, x, y, '▄', Style::new().fg(b).bg(t));
                    }
                    (Some(b), None) => {
                        set_cell(buf, x, y, '▄', Style::new().fg(b));
                    }
                    (None, Some(t)) => {
                        set_cell(buf, x, y, '▀', Style::new().fg(t));
                    }
                }
            }
        }

        // -- X-axis time labels --
        render_x_axis(buf, body_x, xaxis_y, body_width, self.from_ns, self.to_ns);

        // -- Legend (only classes with non-zero AAS) --
        render_legend(buf, area.x, legend_y, area.width as usize, self.result);
    }
}

// -- Helpers ------------------------------------------------------------------

fn set_cell(buf: &mut Buffer, x: u16, y: u16, ch: char, style: Style) {
    if let Some(cell) = buf.cell_mut(Position::new(x, y)) {
        cell.set_char(ch);
        cell.set_style(style);
    }
}

fn render_y_label(buf: &mut Buffer, x: u16, y: u16, value: f64) {
    let label = if value >= 100.0 {
        format!("{:>5.0}", value)
    } else if value >= 10.0 {
        format!("{:>5.1}", value)
    } else {
        format!("{:>5.2}", value)
    };
    let style = Style::new().fg(Color::DarkGray);
    for (i, ch) in label.chars().enumerate().take(5) {
        set_cell(buf, x + i as u16, y, ch, style);
    }
}

fn render_x_axis(buf: &mut Buffer, x: u16, y: u16, width: usize, from_ns: u64, to_ns: u64) {
    if width == 0 || to_ns <= from_ns {
        return;
    }
    let num_labels = (width / 12).max(2).min(8);
    let style = Style::new().fg(Color::DarkGray);

    for i in 0..num_labels {
        let col = if num_labels > 1 {
            i * (width - 1) / (num_labels - 1)
        } else {
            0
        };
        let denom = if width > 1 { (width - 1) as f64 } else { 1.0 };
        let frac = col as f64 / denom;
        let t_ns = from_ns as f64 + frac * (to_ns - from_ns) as f64;
        let dt = chrono::DateTime::from_timestamp_nanos(t_ns as i64);
        let label = dt.format("%H:%M:%S").to_string();

        let start = col.saturating_sub(label.len() / 2);
        for (j, ch) in label.chars().enumerate() {
            let cx = start + j;
            if cx < width {
                set_cell(buf, x + cx as u16, y, ch, style);
            }
        }
    }
}

fn render_legend(buf: &mut Buffer, x: u16, y: u16, width: usize, result: &AasBucketResult) {
    let mut col = 1usize;
    let style_dim = Style::new().fg(Color::Gray);

    for idx in 0..NUM_WAIT_CLASSES {
        let has_data = result.buckets.iter().any(|b| b.class_aas[idx] > 0.0);
        if !has_data {
            continue;
        }

        let label = CLASS_LABELS[idx];
        // Need: 1 (block) + label.len() + 2 (gap)
        if col + 1 + label.len() + 2 > width {
            break;
        }

        // Colored block
        set_cell(buf, x + col as u16, y, '█', Style::new().fg(CLASS_COLORS[idx]));
        col += 1;

        // Label text
        for ch in label.chars() {
            if col < width {
                set_cell(buf, x + col as u16, y, ch, style_dim);
            }
            col += 1;
        }
        col += 2; // gap between entries
    }
}
