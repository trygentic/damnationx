//! DamnationX launcher: Options + Play. Every setting is a diablo.ini `Section.Key=Value`
//! handed to the game as `--set` arguments, and saved in that same form.
//!
//! Desktop: `cargo run --release -- [path/to/devilutionx]` (default: `devilutionx` beside the launcher).
//! Browser: `trunk build` here (settings in Trunk.toml) writes build-webgpu/launcher; the game page
//!          (Packaging/emscripten/index.html) overlays it and Play calls `launch.start(args)`.

use eframe::egui;

/// (ini key, label, default). Section is the part before the first '.'.
const TOGGLES: &[(&str, &str, bool)] = &[
    ("Graphics.HD Tiles", "HD tiles (2x level tiles while zoomed)", true),
    ("Graphics.Hybrid Lighting", "Dynamic lighting (hybrid 2.5D)", true),
    ("Graphics.Per-pixel Lighting", "Per-pixel lighting", true),
    ("Graphics.Ray Traced Lighting", "Ray traced shadows", true),
    ("Graphics.Lighting Bloom", "Lighting bloom", true),
    ("Game.Experience Bar", "Experience bar", true),
    ("Game.Enemy Health Bar", "Enemy health bar", false),
    ("Game.Show health values", "Show health values", false),
    ("Game.Show mana values", "Show mana values", false),
    ("Game.Auto Gold Pickup", "Auto gold pickup", true),
    ("Game.Run in Town", "Run in town", false),
    ("Game.Show Item Labels", "Show item labels", false),
    ("Mods.adria_refills_mana", "Adria refills mana", true),
    ("Mods.permanent_cathedral_portal", "Permanent town portal to Cathedral level 1 (host setting)", false),
    ("Mods.clock", "Clock", false),
    ("Mods.Floating Numbers - Damage", "Floating damage numbers", false),
    ("Mods.Floating Numbers - XP", "Floating XP numbers", false),
];
/// Start zoom levels in quarters (`Graphics.Zoom Level` 1, 1.25 .. 4: screen pixels per game pixel); Ctrl + mouse
/// wheel changes it in game. The game rounds an in-between level to a whole one unless the HD art draws the level.
const ZOOM_QUARTERS: std::ops::RangeInclusive<u8> = 4..=16;

/// "1", "1.25", "1.5", "1.75", "2", .. like the game's diablo.ini.
fn zoom_text(quarters: u8) -> String {
    format!("{}{}", quarters / 4, ["", ".25", ".5", ".75"][usize::from(quarters % 4)])
}

fn parse_zoom(value: &str) -> Option<u8> {
    let level: f32 = value.trim().parse().ok()?;
    let quarters = (level * 4.0).round();
    (quarters.is_finite() && quarters >= 0.0).then(|| (quarters.min(255.0) as u8).clamp(*ZOOM_QUARTERS.start(), *ZOOM_QUARTERS.end()))
}
/// Anything at least twice Diablo's 480 lines gets the full resolution plus 2x zoom.
const HIGH_RES_HEIGHT: u32 = 960;
const RESOLUTIONS: &[(u32, u32)] = &[
    (640, 480), (800, 600), (1024, 768), (1280, 720), (1280, 800),
    (1280, 1024), (1366, 768), (1440, 900), (1600, 900), (1680, 1050),
    (1920, 1080), (1920, 1200), (2560, 1080), (2560, 1440), (2560, 1600),
    (3440, 1440), (3840, 2160), (5120, 2880), (7680, 4320),
];

/// The game resolution for a screen: its exact size (odd sizes are fine) when it has at least
/// HIGH_RES_HEIGHT lines, otherwise Diablo's 480 lines widened to the screen's aspect ratio.
/// Either way the aspect ratio matches the screen, so fullscreen has no black bars.
fn fit_screen((w, h): (u32, u32)) -> (u32, u32) {
    let (w, h) = (w.max(h), w.min(h).max(1));
    let (w, h) = if h >= HIGH_RES_HEIGHT { (w, h) } else { ((w * 480 + h / 2) / h, 480) };
    (w.clamp(640, 7680), h.clamp(480, 4320))
}

struct Launcher {
    width: u32,
    height: u32,
    /// Resolution follows the screen each time the launcher runs (width/height are then derived).
    auto: bool,
    /// Zoom level the game starts at, in quarters (4 = off).
    zoom: u8,
    /// In-game UI scale in quarters (`Graphics.UI Scale`, 4 = 1x), independent of the zoom.
    ui_scale: u8,
    on: Vec<bool>,
    detect: bool,
    options_open: bool,
    saved: String,
    /// Folder holding DIABDAT.MPQ, passed as `--data-dir` (desktop only; the browser uploads instead).
    data_dir: String,
    /// HD art folder, passed as `--hd --hd-dir` (desktop only; needs a WebGPU/Dawn game build).
    /// The browser chooses its HD folder in the page's File Manager instead.
    hd_dir: String,
}

impl Launcher {
    fn defaults() -> Self {
        Self { width: 640, height: 480, auto: true, zoom: 4, ui_scale: 4, on: TOGGLES.iter().map(|t| t.2).collect(), detect: true, options_open: false, saved: String::new(), data_dir: String::new(), hd_dir: String::new() }
    }

    fn args(&self) -> Vec<String> {
        let mut args = vec![
            format!("Graphics.Width={}", self.width),
            format!("Graphics.Height={}", self.height),
            format!("Graphics.Zoom Level={}", zoom_text(self.zoom)),
            format!("Graphics.UI Scale={}", zoom_text(self.ui_scale)),
        ];
        args.extend(TOGGLES.iter().zip(&self.on).map(|((key, ..), on)| format!("{key}={}", *on as u8)));
        args
    }

    fn text(&self) -> String {
        let mut lines = self.args();
        lines.push("LauncherDefaultsVersion=1".to_owned());
        lines.push(format!("AutoResolution={}", self.auto as u8));
        lines.push(format!("DataDir={}", self.data_dir));
        lines.push(format!("HdDir={}", self.hd_dir));
        lines.join("\n")
    }

    fn from_text(text: &str) -> Self {
        let mut s = Self::defaults();
        for (key, value) in text.lines().filter_map(|l| l.split_once('=')) {
            match key {
                "Graphics.Width" => s.width = value.parse().unwrap_or(s.width),
                "Graphics.Height" => s.height = value.parse().unwrap_or(s.height),
                "Graphics.Zoom Level" => s.zoom = parse_zoom(value).unwrap_or(s.zoom),
                "Graphics.UI Scale" => s.ui_scale = parse_zoom(value).unwrap_or(s.ui_scale),
                // Settings saved when zoom was an on/off toggle (on = 2x).
                "Graphics.Zoom" => s.zoom = if value == "1" { 8 } else { 4 },
                // Settings saved before this key existed hold a detected (or stale) size: they get auto.
                "AutoResolution" => s.auto = value == "1",
                "DataDir" => s.data_dir = value.to_owned(),
                "HdDir" => s.hd_dir = value.to_owned(),
                _ => {
                    if let Some(i) = TOGGLES.iter().position(|t| t.0 == key) {
                        s.on[i] = value == "1";
                    }
                }
            }
        }
        // Apply the requested HUD/Adria preset once to older launcher settings.
        // The version is persisted on Play; later custom checkbox choices remain intact.
        if !text.lines().any(|line| line.strip_prefix("LauncherDefaultsVersion=").and_then(|value| value.parse::<u32>().ok()).is_some_and(|version| version >= 1)) {
            for (i, (key, _, default)) in TOGGLES.iter().enumerate() {
                if matches!(*key, "Game.Enemy Health Bar" | "Game.Show health values" | "Game.Show mana values" | "Mods.adria_refills_mana") {
                    s.on[i] = *default;
                }
            }
        }
        s.detect = text.is_empty();
        s.saved = text.to_owned();
        s
    }

    /// First run: start at 2x zoom on high-resolution screens.
    fn apply_screen(&mut self, screen: (u32, u32)) {
        if screen.0.min(screen.1) >= HIGH_RES_HEIGHT {
            self.zoom = 8;
        }
        self.follow_screen(screen);
    }

    fn follow_screen(&mut self, screen: (u32, u32)) {
        if self.auto {
            (self.width, self.height) = fit_screen(screen);
        }
    }
}

impl eframe::App for Launcher {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        if let Some(size) = screen_size(ui.ctx()) {
            if self.detect {
                self.apply_screen(size);
                self.detect = false;
            }
            self.follow_screen(size);
        }
        egui::CentralPanel::default().show(ui, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(40.0);
                ui.heading("DamnationX");
                ui.add_space(30.0);
                let big = egui::vec2(200.0, 40.0);
                if ui.add_sized(big, egui::Button::new("Play")).clicked() {
                    if save(&self.text()) { self.saved = self.text(); }
                    play(ui.ctx(), &self.args(), &self.data_dir, &self.hd_dir, (self.width, self.height));
                }
                if ui.add_sized(big, egui::Button::new("Options")).clicked() {
                    self.options_open = !self.options_open;
                }
                if ui.add_sized(big, egui::Button::new("Game files…")).clicked() {
                    pick_game_files(&mut self.data_dir);
                }
                if !cfg!(target_arch = "wasm32") {
                    ui.weak(if self.data_dir.is_empty() { "DIABDAT.MPQ not selected" } else { &self.data_dir });
                    ui.add_space(10.0);
                    if ui.add_sized(big, egui::Button::new("HD folder…")).clicked() {
                        pick_hd_folder(&mut self.hd_dir);
                    }
                    if self.hd_dir.is_empty() {
                        ui.weak("HD art off");
                    } else {
                        ui.weak(&self.hd_dir);
                        if ui.small_button("Clear (HD off)").clicked() {
                            self.hd_dir.clear();
                        }
                    }
                }
            });
        });
        let mut open = self.options_open;
        egui::Window::new("Options").open(&mut open).vscroll(true).show(ui.ctx(), |ui| {
            ui.horizontal(|ui| {
                ui.label("Resolution");
                let current = (self.width, self.height);
                let custom = !RESOLUTIONS.contains(&current);
                let selected = if self.auto {
                    format!("Auto ({} × {})", self.width, self.height)
                } else if custom {
                    format!("{} × {} (saved custom)", self.width, self.height)
                } else {
                    format!("{} × {}", self.width, self.height)
                };
                egui::ComboBox::from_id_salt("resolution")
                    .selected_text(selected)
                    .show_ui(ui, |ui| {
                        if ui.selectable_label(self.auto, "Auto (match screen)").clicked() {
                            self.auto = true;
                        }
                        // Keep older manually saved sizes available instead of silently replacing them.
                        if custom && !self.auto {
                            let _ = ui.selectable_label(true, format!("{} × {} (saved custom)", current.0, current.1));
                        }
                        for &(width, height) in RESOLUTIONS {
                            if ui.selectable_label(!self.auto && current == (width, height), format!("{width} × {height}")).clicked() {
                                self.auto = false;
                                (self.width, self.height) = (width, height);
                            }
                        }
                    });
            });
            ui.horizontal(|ui| {
                ui.label("Zoom");
                egui::ComboBox::from_id_salt("zoom").selected_text(format!("{}x", zoom_text(self.zoom))).show_ui(ui, |ui| {
                    for quarters in ZOOM_QUARTERS {
                        ui.selectable_value(&mut self.zoom, quarters, format!("{}x", zoom_text(quarters)));
                    }
                });
            })
            .response
            .on_hover_text("Zoom level the game starts at. Ctrl + mouse wheel (Cmd on macOS) zooms in and out in game. \
                In-between levels need the HD art (HD folder with tiles and sprites); otherwise the nearest whole level is used.");
            ui.horizontal(|ui| {
                ui.label("UI scale");
                egui::ComboBox::from_id_salt("ui_scale").selected_text(format!("{}x", zoom_text(self.ui_scale))).show_ui(ui, |ui| {
                    for quarters in ZOOM_QUARTERS {
                        ui.selectable_value(&mut self.ui_scale, quarters, format!("{}x", zoom_text(quarters)));
                    }
                });
            })
            .response
            .on_hover_text("Size of the in-game interface (panels, belt, orbs, inventory, stores, text), independent of the zoom. \
                Also in game under Options. Large scales are limited so the interface still fits the screen.");
            for (section, title) in [("Graphics", "Graphics"), ("Game", "Gameplay"), ("Mods", "Mods")] {
                ui.separator();
                ui.strong(title);
                for (i, (key, label, _)) in TOGGLES.iter().enumerate() {
                    if key.split('.').next() == Some(section) {
                        ui.checkbox(&mut self.on[i], *label);
                    }
                }
            }
            ui.separator();
            if ui.button("Reset to defaults").clicked() {
                *self = Self { options_open: true, data_dir: std::mem::take(&mut self.data_dir), ..Self::defaults() };
            }
        });
        self.options_open &= open;
        let text = self.text();
        if !self.detect && text != self.saved {
            if save(&text) { self.saved = text; }
        }
    }
}

// ---- desktop ----

#[cfg(not(target_arch = "wasm32"))]
fn settings_path() -> std::path::PathBuf {
    // ponytail: file beside the launcher; move to a per-user dir if it ships somewhere read-only.
    std::env::current_exe().unwrap().with_file_name("launcher-settings.txt")
}

#[cfg(not(target_arch = "wasm32"))]
fn save(text: &str) -> bool {
    std::fs::write(settings_path(), text).is_ok()
}

#[cfg(not(target_arch = "wasm32"))]
fn screen_size(ctx: &egui::Context) -> Option<(u32, u32)> {
    ctx.input(|i| i.viewport().monitor_size).map(|s| (s.x as u32, s.y as u32))
}

#[cfg(not(target_arch = "wasm32"))]
fn pick_game_files(data_dir: &mut String) {
    let file = rfd::FileDialog::new().set_title("Select DIABDAT.MPQ").add_filter("Diablo data", &["mpq", "MPQ"]).pick_file();
    if let Some(dir) = file.as_deref().and_then(std::path::Path::parent) {
        *data_dir = dir.display().to_string();
    }
}

/// The HD art folder: the "hd" folder itself, or the unzipped pack that contains it.
#[cfg(not(target_arch = "wasm32"))]
fn pick_hd_folder(hd_dir: &mut String) {
    let Some(dir) = rfd::FileDialog::new().set_title("Select the HD art folder").pick_folder() else { return };
    let hd = if dir.join("sprites.txt").is_file() { dir } else { dir.join("hd") };
    if hd.join("sprites.txt").is_file() {
        *hd_dir = hd.display().to_string();
    } else {
        rfd::MessageDialog::new()
            .set_title("No HD art found")
            .set_description("Choose the \"hd\" folder (it contains sprites.txt) or the folder that contains it.")
            .show();
    }
}

#[cfg(not(target_arch = "wasm32"))]
fn play(ctx: &egui::Context, args: &[String], data_dir: &str, hd_dir: &str, _size: (u32, u32)) {
    let game = std::env::args().nth(1).map(Into::into).unwrap_or_else(|| {
        std::env::current_exe().unwrap().with_file_name(if cfg!(windows) { "devilutionx.exe" } else { "devilutionx" })
    });
    let mut cmd = std::process::Command::new(&game);
    // Without this the game turns Hellfire on whenever it finds hellfire.mpq in any search path.
    cmd.arg("--diablo");
    // A game opened by a damnationx:// link starts without arguments; it reuses these.
    cmd.arg("--remember-launch-options");
    if !data_dir.is_empty() {
        cmd.args(["--data-dir", data_dir]);
    }
    if !hd_dir.is_empty() {
        cmd.args(["--hd", "--hd-dir", hd_dir]);
    }
    for arg in args {
        cmd.args(["--set", arg]);
    }
    if let Ok(path) = std::fs::canonicalize(&game) {
        register_url_scheme(&path);
    }
    match cmd.spawn() {
        Ok(_) => ctx.send_viewport_cmd(egui::ViewportCommand::Close),
        Err(e) => eprintln!("could not start {}: {e}", std::path::Path::new(&game).display()),
    }
}

// ---- damnationx://join/<gameId> links (Discord "Open in desktop game") ----
// macOS registers the scheme in the app's Info.plist. Windows and Linux get a per-user handler
// that runs the game with the URL as its argument; the game reuses the launcher's last options.
// Best effort and silent: Play works the same if this fails. UNTESTED on Windows and Linux.

/// `reg add` argument lists for HKCU\Software\Classes\damnationx.
#[cfg(any(test, windows))]
fn windows_url_scheme_commands(game: &str) -> Vec<Vec<String>> {
    let key = r"HKCU\Software\Classes\damnationx";
    let add = |key: String, value: &[&str], data: String| {
        let mut args = vec!["add".to_owned(), key];
        args.extend(value.iter().map(|v| v.to_string()));
        args.extend(["/d".to_owned(), data, "/f".to_owned()]);
        args
    };
    vec![
        add(key.to_owned(), &["/ve"], "URL:DamnationX Protocol".to_owned()),
        add(key.to_owned(), &["/v", "URL Protocol"], String::new()),
        add(format!(r"{key}\shell\open\command"), &["/ve"], format!("\"{game}\" \"%1\"")),
    ]
}

/// The .desktop entry, or None for a path the Exec quoting rules would need escapes for.
#[cfg(any(test, target_os = "linux"))]
fn linux_desktop_entry(game: &str) -> Option<String> {
    if game.contains(['"', '`', '$', '\\', '\n']) {
        return None;
    }
    Some(format!(
        "[Desktop Entry]\nType=Application\nName=DamnationX\nExec=\"{game}\" %u\nMimeType=x-scheme-handler/damnationx;\nNoDisplay=true\nTerminal=false\n"
    ))
}

#[cfg(windows)]
fn register_url_scheme(game: &std::path::Path) {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    // canonicalize gives a \\?\ verbatim path; the shell wants the plain form.
    let game = game.display().to_string();
    let game = game.strip_prefix(r"\\?\").unwrap_or(&game);
    for args in windows_url_scheme_commands(game) {
        let _ = std::process::Command::new("reg").args(args).creation_flags(CREATE_NO_WINDOW).output();
    }
}

#[cfg(target_os = "linux")]
fn register_url_scheme(game: &std::path::Path) {
    let Some(entry) = linux_desktop_entry(&game.display().to_string()) else { return };
    let Some(data) = std::env::var_os("XDG_DATA_HOME")
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join(".local/share")))
    else {
        return;
    };
    let dir = data.join("applications");
    let file = dir.join("damnationx.desktop");
    if std::fs::read_to_string(&file).is_ok_and(|old| old == entry) {
        return;
    }
    if std::fs::create_dir_all(&dir).is_err() || std::fs::write(&file, entry).is_err() {
        return;
    }
    let _ = std::process::Command::new("xdg-mime").args(["default", "damnationx.desktop", "x-scheme-handler/damnationx"]).output();
    let _ = std::process::Command::new("update-desktop-database").arg(&dir).output();
}

#[cfg(not(any(windows, target_os = "linux", target_arch = "wasm32")))]
fn register_url_scheme(_game: &std::path::Path) {}

#[cfg(not(target_arch = "wasm32"))]
fn main() -> eframe::Result {
    let saved = std::fs::read_to_string(settings_path()).unwrap_or_default();
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([420.0, 520.0]),
        ..Default::default()
    };
    eframe::run_native("DamnationX Launcher", options, Box::new(move |_| Ok(Box::new(Launcher::from_text(&saved)))))
}

// ---- browser ----

#[cfg(target_arch = "wasm32")]
const COOKIE: &str = "dxlauncher";

#[cfg(target_arch = "wasm32")]
fn document() -> web_sys::HtmlDocument {
    use eframe::wasm_bindgen::JsCast;
    web_sys::window().unwrap().document().unwrap().dyn_into().unwrap()
}

/// Persist per-site settings without cookie size or expiry limits.
#[cfg(target_arch = "wasm32")]
fn save(text: &str) -> bool {
    if let Some(storage) = web_sys::window().and_then(|w| w.local_storage().ok().flatten()) {
        if storage.set_item(COOKIE, text).is_ok() { return true; }
    }
    let value = js_sys::encode_uri_component(text);
    document().set_cookie(&format!("{COOKIE}={value}; max-age=31536000; path=/; SameSite=Lax")).is_ok()
}

#[cfg(target_arch = "wasm32")]
fn load() -> String {
    if let Some(storage) = web_sys::window().and_then(|w| w.local_storage().ok().flatten()) {
        if let Ok(Some(text)) = storage.get_item(COOKIE) { return text; }
    }
    let cookies = document().cookie().unwrap_or_default();
    let text = cookies.split(';')
        .find_map(|c| c.trim().strip_prefix(COOKIE).and_then(|v| v.strip_prefix('=')))
        .and_then(|v| js_sys::decode_uri_component(v).ok())
        .map(String::from).unwrap_or_default();
    if !text.is_empty() { save(&text); }
    text
}

#[cfg(target_arch = "wasm32")]
fn screen_size(_ctx: &egui::Context) -> Option<(u32, u32)> {
    let screen = web_sys::window()?.screen().ok()?;
    Some((screen.width().ok()? as u32, screen.height().ok()? as u32))
}

/// The game page's File Manager already stores uploaded MPQs in IndexedDB.
#[cfg(target_arch = "wasm32")]
fn pick_game_files(_data_dir: &mut String) {
    use eframe::wasm_bindgen::JsCast;
    if let Some(button) = document().get_element_by_id("fileManagerBtn") {
        button.unchecked_into::<web_sys::HtmlElement>().click();
    }
}

/// The browser chooses its HD folder in the page's File Manager (hd-folder.js).
#[cfg(target_arch = "wasm32")]
fn pick_hd_folder(_hd_dir: &mut String) {}

#[cfg(target_arch = "wasm32")]
fn play(_ctx: &egui::Context, args: &[String], _data_dir: &str, _hd_dir: &str, (w, h): (u32, u32)) {
    use eframe::wasm_bindgen::JsCast;
    // The windowed game frame (Packaging/emscripten/index.html) takes the game's shape instead of 4:3.
    if let Some(root) = document().document_element() {
        let _ = root.unchecked_into::<web_sys::HtmlElement>().style().set_property("--game-aspect", &format!("{w} / {h}"));
    }
    let argv: js_sys::Array = args.iter().flat_map(|a| ["--set", a.as_str()]).map(eframe::wasm_bindgen::JsValue::from).collect();
    let launch = js_sys::Reflect::get(&web_sys::window().unwrap(), &"launch".into()).unwrap();
    let start: js_sys::Function = js_sys::Reflect::get(&launch, &"start".into()).unwrap().into();
    let _ = start.call1(&launch, &argv);
}

#[cfg(target_arch = "wasm32")]
fn main() {
    use eframe::wasm_bindgen::JsCast;
    let canvas = web_sys::window().unwrap().document().unwrap().get_element_by_id("launcher").unwrap();
    let saved = load();
    wasm_bindgen_futures::spawn_local(async move {
        eframe::WebRunner::new()
            .start(canvas.dyn_into().unwrap(), Default::default(), Box::new(move |_| Ok(Box::new(Launcher::from_text(&saved)))))
            .await
            .expect("failed to start launcher");
    });
}

#[test]
fn settings_round_trip_and_detect() {
    let mut s = Launcher::defaults();
    s.apply_screen((1366, 768));
    assert_eq!((s.width, s.height, s.zoom), (854, 480, 4));
    s.apply_screen((2560, 1440));
    assert_eq!((s.width, s.height, s.zoom), (2560, 1440, 8));
    s.zoom = 13;
    s.ui_scale = 6;
    s.data_dir = "/games/diablo".into();
    let back = Launcher::from_text(&s.text());
    assert_eq!((back.width, back.height, back.zoom, back.ui_scale, &back.on, back.detect, back.auto), (2560, 1440, 13, 6, &s.on, false, true));
    assert!(s.args().contains(&"Graphics.Zoom Level=3.25".to_owned()));
    assert!(s.args().contains(&"Graphics.UI Scale=1.5".to_owned()));
    assert_eq!(Launcher::defaults().ui_scale, 4);
    assert_eq!((parse_zoom("2"), parse_zoom("1.5"), parse_zoom("9"), parse_zoom("x")), (Some(8), Some(6), Some(16), None));
    assert_eq!(back.data_dir, "/games/diablo");
    assert!(Launcher::from_text("").detect);
}

#[test]
fn auto_resolution_matches_screen_shape() {
    // MacBook Pro 16" default scaled mode: odd, not 16:9.
    assert_eq!(fit_screen((1728, 1117)), (1728, 1117));
    assert_eq!(fit_screen((1440, 900)), (768, 480));
    assert_eq!(fit_screen((844, 390)), (1039, 480));
    assert_eq!(fit_screen((390, 844)), (1039, 480));
    assert_eq!(fit_screen((10000, 5000)), (7680, 4320));
    // A cookie from before AutoResolution existed (e.g. a stale 1920x1080) follows the screen.
    let mut old = Launcher::from_text("Graphics.Width=1920\nGraphics.Height=1080\nGraphics.Zoom=1");
    old.follow_screen((1728, 1117));
    assert_eq!((old.auto, old.width, old.height, old.zoom), (true, 1728, 1117, 8));
    // Manual sizes stay put.
    let mut manual = Launcher::from_text("Graphics.Width=1920\nGraphics.Height=1080\nAutoResolution=0");
    manual.follow_screen((1728, 1117));
    assert_eq!((manual.width, manual.height), (1920, 1080));
}

#[test]
fn hud_and_adria_preset_migrates_once() {
    let old = "Game.Enemy Health Bar=1\nGame.Show health values=1\nGame.Show mana values=1\nMods.adria_refills_mana=0";
    let migrated = Launcher::from_text(old);
    for (i, (key, _, default)) in TOGGLES.iter().enumerate() {
        if matches!(*key, "Game.Enemy Health Bar" | "Game.Show health values" | "Game.Show mana values" | "Mods.adria_refills_mana") {
            assert_eq!(migrated.on[i], *default);
        }
    }
    let custom = Launcher::from_text(&format!("LauncherDefaultsVersion=1\n{old}"));
    assert!(custom.on[TOGGLES.iter().position(|t| t.0 == "Game.Enemy Health Bar").unwrap()]);
    assert!(!custom.on[TOGGLES.iter().position(|t| t.0 == "Mods.adria_refills_mana").unwrap()]);
    assert!(migrated.text().contains("LauncherDefaultsVersion=1"));
}

#[test]
fn url_scheme_registration_entries() {
    let cmds = windows_url_scheme_commands(r"C:\Games\DamnationX\devilutionx.exe");
    assert_eq!(cmds[0], ["add", r"HKCU\Software\Classes\damnationx", "/ve", "/d", "URL:DamnationX Protocol", "/f"]);
    assert_eq!(cmds[1], ["add", r"HKCU\Software\Classes\damnationx", "/v", "URL Protocol", "/d", "", "/f"]);
    assert_eq!(
        cmds[2],
        ["add", r"HKCU\Software\Classes\damnationx\shell\open\command", "/ve", "/d", r#""C:\Games\DamnationX\devilutionx.exe" "%1""#, "/f"]
    );
    let entry = linux_desktop_entry("/opt/damnation x/devilutionx").unwrap();
    assert!(entry.contains("Exec=\"/opt/damnation x/devilutionx\" %u\n"));
    assert!(entry.contains("MimeType=x-scheme-handler/damnationx;\n"));
    assert_eq!(linux_desktop_entry("/opt/$HOME/devilutionx"), None);
}
