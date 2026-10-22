//! The fixed .pcraft files of Genko's tests (native/tests/data/pcraft/), written by upstream PhotoCraft's own writer, and
//! a check that a .pcraft (Genko's) loads in upstream's reader. PC4 F-f (docs/cpp-migration/SPEC.md FILE-01).
//!
//! A development tool, never built with Genko. `upstream` beside Cargo.toml is a checkout of PhotoCraft v0.2.0 at the
//! pinned commit (its Cargo.lock is this tool's):
//!
//!     git clone https://github.com/storytold/photocraft upstream
//!     git -C upstream checkout ad863217386440ca968fccc9bfff65ba24e61142
//!     CARGO_TARGET_DIR=<scratch> cargo run -- make ../../../native/tests/data/pcraft
//!     CARGO_TARGET_DIR=<scratch> cargo run -- check <file.pcraft>…   (one JSON line per file: what upstream read)
//!     CARGO_TARGET_DIR=<scratch> cargo run -- dump <file.pcraft>     (its manifest as upstream reads it)
//!
//! Every file is made from fixed numbers (no clock, no randomness), so `make` writes the same bytes again.

use std::path::Path as FsPath;
use std::sync::Arc;

use photocraft_color::{BlendMode, Color, ColorMode, PixelFormat, SampleType};
use photocraft_doc::adjust::{CurvePoint, HueRange, LevelsChannel, ToneSpace};
use photocraft_doc::text::{AntiAlias, Orientation, TextShape};
use photocraft_doc::{
    Adjustment, AlphaChannel, ClippingPath, ColorIndicates, ColorTable, Document, Effect, Fill, FillRule, FxCommon, GradientStyle, Guides, Knot,
    LabelColor, Layer, LayerComp, LayerContent, LayerMask, LineCap, LineJoin, LiveShape, NamedPath, Note, Path, PathOp, Pattern, ShapeLayer, ShapeStroke,
    SmartFilter, SmartObject, SmartSource, StrokeAlign, Subpath, TextLayer, VectorMask,
};
use photocraft_format::{SaveOptions, save_to_bytes};
use photocraft_geom::{Affine, Point, Rect, Size};
use photocraft_raster::{Rgba8Image, Surface};
use serde_json::{Value, json};

/// A value of 0..1 from a pixel's place and a seed (fixed: no randomness).
fn wave(x: i32, y: i32, seed: u32) -> f32 {
    let h = (x as u32).wrapping_mul(73_856_093) ^ (y as u32).wrapping_mul(19_349_663) ^ seed.wrapping_mul(83_492_791);
    (h % 1000) as f32 / 999.0
}

/// Pixels of `s` over the rectangle [x0, x1) × [y0, y1): each channel from `f`.
fn paint(s: &mut Surface, x0: i32, y0: i32, x1: i32, y1: i32, f: impl Fn(i32, i32, usize) -> f32) {
    let n = s.channels();
    for y in y0..y1 {
        for x in x0..x1 {
            let px: Vec<f32> = (0..n).map(|c| f(x, y, c)).collect();
            s.write_pixel(x, y, &px);
        }
    }
}

fn blob(seed: u8, n: usize) -> Arc<Vec<u8>> {
    Arc::new((0..n).map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed)).collect())
}

fn rect_path(x0: f64, y0: f64, x1: f64, y1: f64) -> Path {
    Path::new(vec![Subpath::polygon(&[(x0, y0), (x1, y0), (x1, y1), (x0, y1)])])
}

/// A path of every knot kind and operation (upstream's tests' vector_path, moved onto the canvas).
fn vector_path() -> Path {
    Path::new(vec![
        Subpath {
            closed: true,
            op: PathOp::Combine,
            knots: vec![Knot::corner(41.5, 22.25), Knot::smooth(Point::new(80.0, 25.0), Point::new(70.0, 20.0), Point::new(90.0, 30.0)), Knot::corner(50.0, 80.0)],
        },
        Subpath::polygon(&[(45.0, 25.0), (49.0, 25.0), (49.0, 29.0)]).with_op(PathOp::Subtract),
        Subpath::polyline(&[(40.0, 20.0), (43.0, 24.0)]).with_op(PathOp::Exclude),
        Subpath::polygon(&[(40.0, 20.0), (43.0, 24.0), (43.0, 20.0)]).with_op(PathOp::Intersect),
    ])
}

fn thumb(w: u32, h: u32) -> Rgba8Image {
    let mut t = Rgba8Image::new(w, h);
    for (i, p) in t.pixels.chunks_exact_mut(4).enumerate() {
        p.copy_from_slice(&[(i % 251) as u8, 128, 200, 255]);
    }
    t
}

fn save(doc: &Document, previews: bool) -> Vec<u8> {
    let opts = if previews { SaveOptions { thumbnail: Some(thumb(32, 21)), composite: Some(thumb(64, 43)) } } else { SaveOptions::default() };
    save_to_bytes(doc, &opts).expect("save")
}

/// A small RGB picture as PNG bytes (an embedded smart object's file).
fn png_bytes(w: u32, h: u32) -> Vec<u8> {
    let mut px = Vec::with_capacity((w * h * 4) as usize);
    for y in 0..h {
        for x in 0..w {
            px.extend_from_slice(&[(x * 255 / (w - 1)) as u8, (y * 255 / (h - 1)) as u8, 96, 255]);
        }
    }
    let img = photocraft_codecs::Image::from_u8(w, h, photocraft_codecs::ChannelLayout::Rgba, px).expect("image");
    photocraft_codecs::encode(&img, photocraft_codecs::Format::Png, &Default::default()).expect("png")
}

/// An XMP packet with File Info's author and copyright.
const XMP: &str = concat!(
    "<?xpacket begin=\"\u{feff}\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>",
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">",
    "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">",
    "<dc:creator><rdf:Seq><rdf:li>PhotoCraft Fixture</rdf:li></rdf:Seq></dc:creator>",
    "<dc:rights><rdf:Alt><rdf:li xml:lang=\"x-default\">CC0 test picture</rdf:li></rdf:Alt></dc:rights>",
    "</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>"
);

/// EXIF as a TIFF of one IFD with Orientation 1 (little-endian).
fn exif() -> Arc<Vec<u8>> {
    let mut b = b"II*\0\x08\0\0\0".to_vec();
    b.extend_from_slice(&1u16.to_le_bytes());
    b.extend_from_slice(&0x0112u16.to_le_bytes());
    b.extend_from_slice(&3u16.to_le_bytes());
    b.extend_from_slice(&1u32.to_le_bytes());
    b.extend_from_slice(&1u32.to_le_bytes());
    b.extend_from_slice(&0u32.to_le_bytes());
    Arc::new(b)
}

fn rgb(r: f32, g: f32, b: f32) -> Color {
    Color::rgb(r, g, b)
}

/// The small document a smart object of the rich file holds as a .pcraft of its own.
fn inner_doc() -> Document {
    let mut d = Document::with_background("Inner", Size::new(40, 30), ColorMode::Rgb, SampleType::U8, rgb(0.9, 0.9, 0.2));
    d.resolution_dpi = 300.0;
    let mut l = Layer::raster("Dot", d.pixel_format());
    paint(l.surface_mut().unwrap(), 10, 10, 30, 20, |x, y, c| if c == 3 { 1.0 } else { wave(x, y, c as u32 + 40) });
    d.layers.push(l);
    d
}

/// RGB 8 bits, every layer kind and what a page keeps beside its layers.
fn rich() -> Document {
    let mut d = Document::with_background("Rich", Size::new(300, 200), ColorMode::Rgb, SampleType::U8, Color::WHITE);
    d.resolution_dpi = 300.0;
    let pf = d.pixel_format();

    let mut p = Layer::raster("Paint", pf);
    {
        let s = p.surface_mut().unwrap();
        paint(s, 20, 30, 120, 90, |x, y, c| if c == 3 { 0.5 + 0.5 * wave(x, y, 9) } else { wave(x, y, c as u32) });
        // pixels off the canvas (left above, and right of it): cut when read
        paint(s, -6, -6, 4, 4, |_, _, c| if c == 0 || c == 3 { 1.0 } else { 0.0 });
        paint(s, 296, 10, 320, 20, |_, _, c| if c == 2 || c == 3 { 1.0 } else { 0.0 });
    }
    p.blend = BlendMode::Multiply;
    p.opacity = 0.75;
    p.label = LabelColor::Violet;
    p.locks.pixels = true;
    let mut mask = LayerMask::reveal_all();
    paint(&mut mask.surface, 60, 40, 100, 70, |x, _, _| if x < 80 { 0.0 } else { 0.5 });
    mask.density = 0.8;
    mask.feather = 3.5;
    mask.linked = false;
    p.mask = Some(mask);
    let mut vm = VectorMask::new(rect_path(10.0, 20.0, 140.0, 95.0));
    vm.density = 0.9;
    vm.feather = 2.0;
    p.vector_mask = Some(vm);
    p.psd_blocks.push((*b"lnsr", blob(5, 4)));
    p.psd_id = Some(42);
    p.link_group = Some(5);

    let mut clipped = Layer::raster("Clipped", pf);
    paint(clipped.surface_mut().unwrap(), 30, 40, 70, 60, |_, _, c| [0.0, 0.6, 0.2, 1.0][c]);
    clipped.clipped = true;
    clipped.link_group = Some(5);

    let mut inner = Layer::raster("Inner", pf);
    paint(inner.surface_mut().unwrap(), 150, 20, 190, 60, |x, y, c| if c == 3 { 1.0 } else { wave(x, y, c as u32 + 20) });
    let nested = Layer::group("Nested", vec![inner]);
    let mut hue = Layer::new(
        "Hue/Sat",
        LayerContent::Adjustment(Adjustment::HueSaturation { hue: 30.0, saturation: -10.0, lightness: 5.0, colorize: false, ranges: HueRange::defaults() }),
    );
    hue.visible = false;
    let mut group = Layer::group("Group", vec![nested, hue]);
    if let LayerContent::Group(g) = &mut group.content {
        g.expanded = false;
    }
    group.opacity = 0.9;

    let mut solid = Layer::new("Solid", LayerContent::Fill(Fill::Solid(Color::rgba(0.1, 0.2, 0.3, 1.0))));
    let mut smask = LayerMask::hide_all();
    paint(&mut smask.surface, 200, 150, 260, 190, |_, _, _| 1.0);
    solid.mask = Some(smask);
    solid.opacity = 0.6;
    let gradient = Layer::new(
        "Gradient",
        LayerContent::Fill(Fill::Gradient {
            stops: vec![(0.0, Color::BLACK), (1.0, Color::WHITE)],
            angle: 45.0,
            scale: 1.0,
            style: GradientStyle::Linear,
            reverse: false,
        }),
    );
    let mut gradient = gradient;
    gradient.visible = false;
    let mut pat = Surface::new(pf);
    paint(&mut pat, 0, 0, 8, 8, |x, y, c| if c == 3 { 1.0 } else if (x + y) % 2 == 0 { 1.0 } else { 0.2 * c as f32 });
    d.patterns.push(Pattern::new("Checks", pat, 8, 8));
    let pattern_id = d.patterns[0].id.clone();
    let mut pattern = Layer::new(
        "Pattern",
        LayerContent::Fill(Fill::Pattern { name: "Checks".into(), scale: 1.0, id: pattern_id, angle: 0.0, link: true, phase: (0.0, 0.0) }),
    );
    pattern.visible = false;

    let mut tcache = Surface::new(pf);
    paint(&mut tcache, 20, 150, 90, 170, |x, y, c| if c == 3 { if (x + y) % 3 == 0 { 1.0 } else { 0.0 } } else { 0.1 });
    let text = Layer::new(
        "Type",
        LayerContent::Text(TextLayer {
            text: "Hello".into(),
            font_family: "Inter".into(),
            size_pt: 12.0,
            color: rgb(0.1, 0.1, 0.1),
            transform: Affine::translate(20.0, 165.0),
            cache: Some(tcache),
            psd_raw: None,
            runs: vec![],
            paragraphs: vec![],
            shape: TextShape::Point,
            orientation: Orientation::Horizontal,
            antialias: AntiAlias::Smooth,
            warp: None,
        }),
    );

    let mut scache = Surface::new(pf);
    paint(&mut scache, 200, 20, 260, 70, |_, _, c| [0.9, 0.1, 0.1, 1.0][c]);
    let mut shape = Layer::new(
        "Shape",
        LayerContent::Shape(ShapeLayer {
            path: rect_path(200.0, 20.0, 260.0, 70.0),
            fill: Some(Fill::Solid(rgb(0.9, 0.1, 0.1))),
            stroke: Some(ShapeStroke {
                width: 2.0,
                paint: Fill::Solid(rgb(0.0, 0.0, 0.5)),
                opacity: 1.0,
                align: StrokeAlign::Center,
                cap: LineCap::Butt,
                join: LineJoin::Miter,
                miter_limit: 4.0,
                dashes: vec![],
                dash_offset: 0.0,
            }),
            live: Some(LiveShape::Rect { rect: [200.0, 20.0, 60.0, 50.0], radii: [0.0; 4] }),
            cache: Some(scache),
            psd_raw: None,
        }),
    );
    shape.label = LabelColor::Green;

    let mut sofile = Surface::new(pf);
    paint(&mut sofile, 150, 100, 214, 132, |x, y, c| if c == 3 { 1.0 } else { wave(x, y, c as u32 + 3) });
    let mut smart = SmartObject::new(
        SmartSource::Embedded { file_name: "inner.png".into(), bytes: Arc::new(png_bytes(32, 16)) },
        Affine { m: [2.0, 0.0, 0.0, 2.0, 150.0, 100.0] },
        Some(sofile),
    );
    smart.smart_filters.push(SmartFilter {
        command: "filter.blur.gaussianBlur".into(),
        params: json!({"radius": 2.0}),
        blend: BlendMode::Normal,
        opacity: 1.0,
        visible: true,
    });
    smart.smart_filters.push(SmartFilter { command: "filter.unknown.later".into(), params: json!({}), blend: BlendMode::Normal, opacity: 1.0, visible: true });
    let smart = Layer::new("Smart", LayerContent::Smart(smart));

    let mut lcache = Surface::new(pf);
    paint(&mut lcache, 100, 120, 130, 140, |_, _, c| [0.2, 0.2, 0.8, 1.0][c]);
    let linked = Layer::new("Linked", LayerContent::Smart(SmartObject::new(SmartSource::Linked { path: "/tmp/linked.png".into() }, Affine::IDENTITY, Some(lcache))));

    let mut ncache = Surface::new(pf);
    paint(&mut ncache, 240, 120, 280, 150, |_, _, c| [0.9, 0.9, 0.2, 1.0][c]);
    let nested_pcraft = save(&inner_doc(), false);
    let nested_so = Layer::new(
        "Nested",
        LayerContent::Smart(SmartObject::new(
            SmartSource::Embedded { file_name: "inner.pcraft".into(), bytes: Arc::new(nested_pcraft) },
            Affine::translate(240.0, 120.0),
            Some(ncache),
        )),
    );

    let mut fx = Layer::raster("Effects", pf);
    paint(fx.surface_mut().unwrap(), 130, 160, 170, 190, |_, _, c| [0.5, 0.5, 0.5, 1.0][c]);
    fx.fill_opacity = 0.5;
    fx.effects.items.push(Effect::ColorOverlay { common: FxCommon::new(BlendMode::Normal, 0.8), color: rgb(1.0, 0.2, 0.1) });
    fx.excluded_channels = 1;

    d.layers.extend([p, clipped, group, solid, gradient, pattern, text, shape, smart, linked, nested_so, fx]);

    let mut ch = Surface::new(PixelFormat::GRAY8);
    paint(&mut ch, 10, 10, 60, 40, |x, _, _| (x - 10) as f32 / 50.0);
    let mut alpha = AlphaChannel::new("Alpha 1", ch.clone());
    alpha.color = rgb(0.0, 0.5, 1.0);
    alpha.opacity = 0.35;
    alpha.indicates = ColorIndicates::SelectedAreas;
    d.channels.push(alpha);
    d.channels.push(AlphaChannel { spot: Some((rgb(1.0, 0.0, 0.5), 0.7)), ..AlphaChannel::new("Spot", ch.clone()) });
    d.quick_mask = Some(AlphaChannel::new("Quick Mask", ch));
    d.guides = Guides { horizontal: vec![10.0, 20.5], vertical: vec![100.25] };
    let mut sel = Surface::new(PixelFormat::GRAY8);
    paint(&mut sel, 0, 0, 10, 10, |_, _, _| 1.0);
    d.selection = Some(sel);
    d.metadata.xmp = Some(XMP.into());
    d.metadata.exif = Some(exif());
    d.metadata.psd_resources.push((1037, String::new(), Arc::new(vec![0, 0, 0, 120])));
    d.metadata.psd_global_blocks.push((*b"8BIM", *b"Anno", blob(17, 50)));
    d.paths.push(NamedPath { name: "Path 1".into(), path: vector_path(), psd_raw: None });
    d.paths.push(NamedPath { name: "Empty".into(), path: Path::default(), psd_raw: None });
    let mut wp = rect_path(5.0, 5.0, 25.0, 15.0);
    wp.fill_rule = FillRule::EvenOdd;
    d.work_path = Some(wp);
    d.clipping_path = Some(ClippingPath { name: "Path 1".into(), flatness: 2.0 });
    d.layer_comps.push(LayerComp {
        id: 1,
        name: "Comp".into(),
        comment: String::new(),
        apply_visibility: true,
        apply_position: false,
        apply_appearance: false,
        states: vec![],
    });
    d.notes.push(Note { author: "A".into(), text: "note".into(), ..Default::default() });
    d
}

fn levels_ch(b: f32, w: f32, g: f32) -> LevelsChannel {
    LevelsChannel { in_black: b, in_white: w, gamma: g, out_black: 0.0, out_white: 1.0 }
}

fn curve(points: &[(f32, f32)]) -> Vec<CurvePoint> {
    points.iter().map(|&(i, o)| CurvePoint { input: i, output: o }).collect()
}

/// RGB 8 bits, a gradient under one adjustment layer of each kind (all hidden but the first, so each can be shown alone).
fn adjustments() -> Document {
    let mut d = Document::new("Adjustments", Size::new(64, 32), ColorMode::Rgb, SampleType::U8);
    d.resolution_dpi = 150.0;
    let mut bg = Layer::raster("Background", d.pixel_format());
    paint(bg.surface_mut().unwrap(), 0, 0, 64, 32, |x, y, c| if c == 3 { 1.0 } else { [x as f32 / 63.0, y as f32 / 31.0, 0.5][c] });
    d.layers.push(bg);
    let mut ranges = HueRange::defaults();
    ranges[2].hue = -20.0;
    ranges[2].saturation = 15.0;
    let lut: Vec<f32> = (0..8).flat_map(|i| [(i & 1) as f32, ((i >> 1) & 1) as f32 * 0.8, ((i >> 2) & 1) as f32]).collect();
    let kinds: Vec<(&str, Adjustment)> = vec![
        ("Brightness", Adjustment::BrightnessContrast { brightness: 20.0, contrast: -10.0, legacy: false }),
        ("Levels", Adjustment::Levels { master: levels_ch(0.1, 0.9, 1.2), per_channel: [levels_ch(0.0, 1.0, 0.8), LevelsChannel::default(), LevelsChannel::default()], space: ToneSpace::Rgb, black: LevelsChannel::default() }),
        ("Curves", Adjustment::Curves { master: curve(&[(0.0, 0.0), (0.5, 0.6), (1.0, 1.0)]), per_channel: [curve(&[]), curve(&[(0.0, 0.1), (1.0, 0.9)]), curve(&[])], space: ToneSpace::Rgb, black: vec![] }),
        ("Exposure", Adjustment::Exposure { exposure: 0.5, offset: -0.02, gamma: 1.1 }),
        ("Vibrance", Adjustment::Vibrance { vibrance: 30.0, saturation: -5.0 }),
        ("HueSat", Adjustment::HueSaturation { hue: 10.0, saturation: 20.0, lightness: -5.0, colorize: false, ranges }),
        ("Balance", Adjustment::ColorBalance { shadows: [10.0, 0.0, -10.0], midtones: [0.0, 5.0, 0.0], highlights: [-5.0, 0.0, 5.0], preserve_luminosity: true }),
        ("BlackWhite", Adjustment::BlackWhite { weights: [40.0, 60.0, 40.0, 60.0, 20.0, 80.0], tint: Some([0.882, 0.827, 0.702]) }),
        ("PhotoFilter", Adjustment::PhotoFilter { color: [236.0 / 255.0, 138.0 / 255.0, 0.0], density: 0.25, preserve_luminosity: true }),
        ("Mixer", Adjustment::ChannelMixer { matrix: [[0.8, 0.2, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.1, 0.9, 0.05]], monochrome: false }),
        ("Lookup", Adjustment::ColorLookup { name: "Swap".into(), lut: Some(Arc::new(lut)), size: 2, tetrahedral: false, dither: false }),
        ("Invert", Adjustment::Invert),
        ("Posterize", Adjustment::Posterize { levels: 4 }),
        ("Threshold", Adjustment::Threshold { level: 0.5 }),
        ("GradientMap", Adjustment::GradientMap { stops: vec![(0.0, [0.0, 0.0, 0.3]), (1.0, [1.0, 0.9, 0.6])], reverse: false, dither: false }),
        ("Selective", Adjustment::SelectiveColor { relative: true, adjustments: [[10.0, 0.0, 0.0, 0.0], [0.0; 4], [0.0; 4], [0.0; 4], [0.0; 4], [0.0; 4], [0.0; 4], [0.0, 0.0, 0.0, 5.0], [0.0; 4]] }),
        ("Unknown", Adjustment::Unsupported { psd_key: "xxxx".into(), raw: vec![1, 2, 3] }),
    ];
    for (i, (name, a)) in kinds.into_iter().enumerate() {
        let mut l = Layer::new(name, LayerContent::Adjustment(a));
        l.visible = i == 0;
        d.layers.push(l);
    }
    d
}

/// A document of `mode` and `depth`: a background and a layer of known values with a mask, and an alpha channel.
fn plain(name: &str, mode: ColorMode, depth: SampleType) -> Document {
    let mut d = Document::new(name, Size::new(96, 64), mode, depth);
    d.resolution_dpi = 200.0;
    let pf = d.pixel_format();
    let colours = pf.channels() - 1;
    let hdr = depth == SampleType::F32;
    let mut bg = Layer::raster("Background", pf);
    paint(bg.surface_mut().unwrap(), 0, 0, 96, 64, |x, y, c| {
        if c == colours {
            1.0
        } else if hdr {
            // linear light past 1 and below 0
            (x as f32 / 95.0) * 3.0 - 0.5 + c as f32 * 0.125 + y as f32 / 640.0
        } else {
            let v = (x as f32 * 37.0 + y as f32 * 11.0 + c as f32 * 101.0) % 1000.0 / 999.0;
            if mode == ColorMode::Indexed { if v < 0.5 { 0.0 } else { 1.0 } } else { v }
        }
    });
    d.layers.push(bg);
    let mut l = Layer::raster("Layer 1", pf);
    paint(l.surface_mut().unwrap(), 16, 8, 80, 56, |x, y, c| {
        if c == colours {
            0.25 + 0.75 * wave(x, y, 77)
        } else if mode == ColorMode::Indexed {
            [1.0, 0.0, 0.0][c]
        } else {
            wave(x, y, c as u32 + 7) * if hdr { 1.5 } else { 1.0 }
        }
    });
    let mut mask = LayerMask::reveal_all();
    paint(&mut mask.surface, 40, 20, 60, 40, |_, _, _| 0.0);
    l.mask = Some(mask);
    l.opacity = 0.8;
    l.blend = BlendMode::Screen;
    d.layers.push(l);
    let mut ch = Surface::new(PixelFormat::GRAY8.with_sample(depth));
    paint(&mut ch, 0, 0, 48, 32, |x, y, _| ((x + y) % 7) as f32 / 6.0);
    d.channels.push(AlphaChannel::new("Alpha 1", ch));
    if mode == ColorMode::Indexed {
        d.color_table = Some(ColorTable { colors: vec![[0, 0, 0], [255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255], [0, 255, 255], [255, 0, 255], [255, 255, 0]], transparent: None });
    }
    if mode == ColorMode::Cmyk {
        d.layers.push(Layer::new(
            "Curves",
            LayerContent::Adjustment(Adjustment::Curves {
                master: curve(&[(0.0, 0.0), (1.0, 1.0)]),
                per_channel: [curve(&[(0.0, 0.0), (0.5, 0.4), (1.0, 1.0)]), curve(&[]), curve(&[])],
                space: ToneSpace::Cmyk,
                black: curve(&[(0.0, 0.0), (1.0, 0.9)]),
            }),
        ));
    }
    d
}

fn make(dir: &FsPath) {
    std::fs::create_dir_all(dir).expect("folder");
    let files: Vec<(&str, Vec<u8>)> = vec![
        ("rich_rgb8.pcraft", save(&rich(), true)),
        ("adjustments.pcraft", save(&adjustments(), false)),
        ("rgb16.pcraft", save(&plain("RGB 16", ColorMode::Rgb, SampleType::U16), false)),
        ("rgb32.pcraft", save(&plain("RGB 32", ColorMode::Rgb, SampleType::F32), false)),
        ("gray8.pcraft", save(&plain("Gray 8", ColorMode::Grayscale, SampleType::U8), false)),
        ("gray16.pcraft", save(&plain("Gray 16", ColorMode::Grayscale, SampleType::U16), false)),
        ("cmyk8.pcraft", save(&plain("CMYK 8", ColorMode::Cmyk, SampleType::U8), false)),
        ("lab8.pcraft", save(&plain("Lab 8", ColorMode::Lab, SampleType::U8), false)),
        ("indexed.pcraft", save(&plain("Indexed", ColorMode::Indexed, SampleType::U8), false)),
    ];
    for (name, bytes) in files {
        std::fs::write(dir.join(name), &bytes).expect("write");
        println!("{name} {}", bytes.len());
    }
}

fn kind_of(l: &Layer) -> Value {
    match &l.content {
        LayerContent::Raster(s) => json!({"kind": "raster", "tiles": s.tile_count()}),
        LayerContent::Group(g) => json!({"kind": "group", "children": g.children.iter().map(layer_json).collect::<Vec<_>>()}),
        LayerContent::Adjustment(a) => json!({"kind": "adjustment", "label": a.label()}),
        LayerContent::Fill(_) => json!({"kind": "fill"}),
        LayerContent::Text(t) => json!({"kind": "text", "text": t.text, "cache": t.cache.is_some()}),
        LayerContent::Shape(s) => json!({"kind": "shape", "subpaths": s.path.subpaths.len(), "cache": s.cache.is_some()}),
        LayerContent::Smart(s) => json!({"kind": "smart", "source": match &s.source { SmartSource::Embedded { file_name, bytes } => json!({"embedded": file_name, "bytes": bytes.len()}), SmartSource::Linked { path } => json!({"linked": path}) }}),
    }
}

fn layer_json(l: &Layer) -> Value {
    let mut v = kind_of(l);
    v["name"] = json!(l.name);
    v["visible"] = json!(l.visible);
    v["opacity"] = json!(l.opacity);
    v["blend"] = serde_json::to_value(l.blend).unwrap_or(Value::Null);
    v["clipped"] = json!(l.clipped);
    v["mask"] = json!(l.mask.is_some());
    v["vector_mask"] = json!(l.vector_mask.is_some());
    v
}

/// What upstream read of a .pcraft: one JSON line.
fn check(path: &FsPath) -> Value {
    match photocraft_format::load_path(path) {
        Ok(d) => {
            let bounds = Rect::from_size(d.size);
            json!({
                "file": path.display().to_string(), "ok": true, "name": d.name, "width": d.size.width, "height": d.size.height,
                "dpi": d.resolution_dpi, "mode": serde_json::to_value(d.mode).unwrap_or(Value::Null),
                "depth": serde_json::to_value(d.depth).unwrap_or(Value::Null), "icc": d.icc_profile.as_ref().map(|b| b.len()),
                "layers": d.layers.iter().map(layer_json).collect::<Vec<_>>(),
                "channels": d.channels.iter().map(|c| json!({"name": c.name, "spot": c.spot.is_some()})).collect::<Vec<_>>(),
                "paths": d.paths.iter().map(|p| p.name.clone()).collect::<Vec<_>>(), "work_path": d.work_path.is_some(),
                "clipping_path": d.clipping_path.as_ref().map(|c| c.name.clone()),
                "guides": [d.guides.horizontal.len(), d.guides.vertical.len()], "patterns": d.patterns.len(),
                "xmp": d.metadata.xmp.is_some(), "exif": d.metadata.exif.is_some(),
                "first_pixels": d.layers.first().and_then(|l| l.surface()).map(|s| s.pixel(bounds.x0, bounds.y0)),
            })
        }
        Err(e) => json!({"file": path.display().to_string(), "ok": false, "error": e.to_string()}),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("make") if args.len() == 3 => make(FsPath::new(&args[2])),
        Some("check") if args.len() >= 3 => {
            let mut failed = false;
            for p in &args[2..] {
                let v = check(FsPath::new(p));
                failed |= v["ok"] != json!(true);
                println!("{v}");
            }
            if failed {
                std::process::exit(1);
            }
        }
        Some("dump") if args.len() == 3 => {
            let bytes = std::fs::read(&args[2]).expect("read");
            let m = photocraft_format::read_manifest(&bytes).expect("manifest");
            println!("{}", serde_json::to_string_pretty(&m).expect("json"));
        }
        _ => {
            eprintln!("usage: pcraft-fixtures make <folder> | check <file>… | dump <file>");
            std::process::exit(2);
        }
    }
}
