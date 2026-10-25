//! Native launcher. Asset work runs outside egui; the web frontend stays independent.
use crate::assets::{self, AssetCandidates, AssetPaths, AssetReadiness, AssetStatus, CachePaths};
use crate::d2r_cache;
use crate::jobs::{
    D2rImportRequest, D2rSoundImportRequest, JobEvent, JobHandle, JobOutcome, JobRequest,
};
use crate::{egui, Launcher, RESOLUTIONS, TOGGLES};
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use std::sync::mpsc;
use std::time::Duration;

const GOLD: egui::Color32 = egui::Color32::from_rgb(207, 173, 104);
const IVORY: egui::Color32 = egui::Color32::from_rgb(227, 218, 196);
const MUTED: egui::Color32 = egui::Color32::from_rgb(155, 153, 144);
const GREEN: egui::Color32 = egui::Color32::from_rgb(132, 185, 152);
const ERROR: egui::Color32 = egui::Color32::from_rgb(236, 145, 129);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
enum Runtime {
    #[default]
    #[serde(alias = "Bevy")]
    Toxoid,
    Classic,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum SoundPack {
    #[default]
    Original,
    D2rClassic,
    D2rRemastered,
}

impl SoundPack {
    fn label(self) -> &'static str {
        match self {
            Self::Original => "Diablo (original)",
            Self::D2rClassic => "D2R Classic",
            Self::D2rRemastered => "D2R Remastered",
        }
    }

    fn native_value(self) -> u8 {
        match self {
            Self::Original => 0,
            Self::D2rClassic => 2,
            Self::D2rRemastered => 3,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
struct Settings {
    version: u32,
    game: PathBuf,
    runtime: Runtime,
    data_dir: Option<PathBuf>,
    hd_dir: Option<PathBuf>,
    d2r_dir: Option<PathBuf>,
    d2r_install: Option<PathBuf>,
    importer: Option<PathBuf>,
    python: PathBuf,
    managed_python: bool,
    use_hd: bool,
    /// Master switch for everything imported from D2R (art, interface, sounds).
    /// Off restores original visuals and audio without touching the import.
    use_d2r: bool,
    use_models: bool,
    use_environment: bool,
    resolution: [u32; 2],
    auto_resolution: bool,
    render_mode: String,
    rt: bool,
    rt_quality: String,
    sound_pack: SoundPack,
    legacy_options: String,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            version: 2,
            game: PathBuf::new(),
            runtime: Runtime::Toxoid,
            data_dir: None,
            hd_dir: None,
            d2r_dir: None,
            d2r_install: None,
            importer: None,
            python: PathBuf::from(if cfg!(windows) {
                "python.exe"
            } else {
                "python3"
            }),
            managed_python: true,
            use_hd: true,
            use_d2r: true,
            use_models: true,
            use_environment: true,
            resolution: [1280, 720],
            auto_resolution: false,
            render_mode: "3d".into(),
            rt: false,
            rt_quality: "low".into(),
            sound_pack: SoundPack::Original,
            legacy_options: Launcher::defaults().text(),
        }
    }
}

impl Settings {
    fn paths(&self) -> AssetPaths {
        AssetPaths {
            game: (!self.game.as_os_str().is_empty()).then(|| self.game.clone()),
            data_dir: self.data_dir.clone(),
            hd_dir: self.hd_dir.clone(),
            d2r_dir: self.d2r_dir.clone(),
            sound_mods: None,
        }
    }

    fn sanitized(mut self) -> Self {
        self.version = 2;
        self.resolution[0] = self.resolution[0].clamp(640, 7680);
        self.resolution[1] = self.resolution[1].clamp(480, 4320);
        if !["3d", "classic", "hd"].contains(&self.render_mode.as_str()) {
            self.render_mode = "3d".into();
        }
        if !["low", "medium", "high", "ultra"].contains(&self.rt_quality.as_str()) {
            self.rt_quality = "low".into();
        }
        self
    }
}

#[derive(Debug, Default)]
struct Cli {
    game: Option<PathBuf>,
    settings: Option<PathBuf>,
    page: usize,
    help: bool,
    trainer: Option<PathBuf>,
    import_d2r_sounds: Option<PathBuf>,
}

fn cli(args: impl IntoIterator<Item = OsString>) -> Result<Cli, String> {
    let mut result = Cli::default();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--help" | "-h") => result.help = true,
            Some("--game") => {
                result.game = Some(args.next().ok_or("--game needs an executable path")?.into())
            }
            Some("--settings") => {
                result.settings = Some(args.next().ok_or("--settings needs a file path")?.into())
            }
            Some("--page") => {
                result.page = match args.next().as_deref().and_then(OsStr::to_str) {
                    Some("library") => 0,
                    Some("options") => 1,
                    Some("sounds") => 2,
                    _ => return Err("--page needs library, options or sounds".into()),
                };
            }
            Some("--import-d2r-sounds") => {
                result.import_d2r_sounds = Some(
                    args.next()
                        .ok_or("--import-d2r-sounds needs an owned D2R installation folder")?
                        .into(),
                );
            }
            _ if arg.to_string_lossy().starts_with('-') => {
                return Err(format!("Unknown option: {}", arg.to_string_lossy()))
            }
            _ if result.game.is_none() => result.game = Some(arg.into()),
            _ if cfg!(feature = "trainer") && result.trainer.is_none() => {
                result.trainer = Some(arg.into())
            }
            _ => return Err("Only one game executable can be selected".into()),
        }
    }
    Ok(result)
}

fn runtime_for(path: &Path) -> Runtime {
    if path.file_stem().is_some_and(|s| {
        s.to_string_lossy()
            .to_ascii_lowercase()
            .starts_with("devilutionx")
    }) {
        Runtime::Classic
    } else {
        Runtime::Toxoid
    }
}

fn game_executable(path: &Path) -> PathBuf {
    if cfg!(target_os = "macos")
        && path
            .extension()
            .is_some_and(|s| s.eq_ignore_ascii_case("app"))
    {
        for name in ["damnationx_host", "devilutionx", "DamnationX"] {
            let file = path.join("Contents/MacOS").join(name);
            if file.is_file() {
                return file;
            }
        }
    }
    path.to_path_buf()
}

fn nearby_game() -> Option<PathBuf> {
    let parent = std::env::current_exe().ok()?.parent()?.to_path_buf();
    for root in [
        parent.clone(),
        parent.join("bin"),
        parent.join("../Resources"),
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../target/release"),
    ] {
        for name in if cfg!(windows) {
            ["damnationx_host.exe", "devilutionx.exe"]
        } else {
            ["damnationx_host", "devilutionx"]
        } {
            let path = root.join(name);
            if path.is_file() {
                return Some(path);
            }
        }
        for name in ["DamnationX.app", "devilutionx.app"] {
            let app = root.join(name);
            let game = game_executable(&app);
            if game.is_file() {
                return Some(game);
            }
        }
    }
    None
}

fn load_settings(path: &Path, old_path: Option<&Path>) -> (Settings, Option<String>) {
    match std::fs::read_to_string(path) {
        Ok(text) => match serde_json::from_str::<Settings>(&text) {
            Ok(settings) => (settings.sanitized(), None),
            Err(e) => (
                Settings::default(),
                Some(format!("Could not read saved launcher settings: {e}")),
            ),
        },
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            if let Some(text) = old_path.and_then(|p| std::fs::read_to_string(p).ok()) {
                let old = Launcher::from_text(&text);
                let settings = Settings {
                    data_dir: (!old.data_dir.is_empty()).then(|| old.data_dir.into()),
                    hd_dir: (!old.hd_dir.is_empty()).then(|| old.hd_dir.into()),
                    resolution: [old.width, old.height],
                    auto_resolution: old.auto,
                    legacy_options: text,
                    ..Default::default()
                };
                return (settings.sanitized(), None);
            }
            (Settings::default(), None)
        }
        Err(e) => (
            Settings::default(),
            Some(format!("Could not open launcher settings: {e}")),
        ),
    }
}

fn write_settings(path: &Path, text: &str) -> Result<(), String> {
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let temp = parent.join(format!(".launcher-{}-settings.tmp", std::process::id()));
    std::fs::write(&temp, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temp, path).map_err(|e| {
        let _ = std::fs::remove_file(&temp);
        e.to_string()
    })
}

/// The unattended import changes only the authorized sound selection (and the
/// D2R switch, so the selection is heard). Preserve
/// every other JSON value, including legacy aliases and newer unknown fields.
fn write_sound_selection(path: &Path, fallback: &Settings) -> Result<(), String> {
    let mut value: serde_json::Value = match std::fs::read_to_string(path) {
        Ok(text) => serde_json::from_str(&text)
            .map_err(|e| format!("Cannot update existing settings: {e}"))?,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            serde_json::to_value(fallback).map_err(|e| e.to_string())?
        }
        Err(e) => return Err(e.to_string()),
    };
    let object = value
        .as_object_mut()
        .ok_or("Launcher settings must be a JSON object")?;
    object.insert(
        "sound_pack".into(),
        serde_json::to_value(SoundPack::D2rRemastered).map_err(|e| e.to_string())?,
    );
    // Absent means on; only a stored "off" needs rewriting.
    if let Some(switch) = object.get_mut("use_d2r") {
        *switch = serde_json::Value::Bool(true);
    }
    write_settings(
        path,
        &serde_json::to_string_pretty(&value).map_err(|e| e.to_string())?,
    )
}

fn sound_import_request(
    settings: &Settings,
    paths: &CachePaths,
    source: PathBuf,
) -> Result<D2rSoundImportRequest, String> {
    let program = game_executable(&settings.game);
    if !program.is_file() {
        return Err("Choose the game executable in Launch options before importing sounds".into());
    }
    Ok(D2rSoundImportRequest {
        program,
        game_dir: source,
        destination: paths.sound_mods.clone(),
        classic_runtime: settings.runtime == Runtime::Classic,
        data_dir: settings.data_dir.clone(),
        stamp: Some(paths.d2r_cache.join(d2r_cache::SOUND_STAMP)),
    })
}

fn import_sounds_without_window(request: D2rSoundImportRequest) -> Result<(), String> {
    let job = JobHandle::spawn(JobRequest::ImportD2rSounds(request));
    loop {
        for event in job.poll() {
            match event {
                JobEvent::Log(line) => println!("{line}"),
                JobEvent::Progress { phase, done, total } => {
                    if let Some(total) = total {
                        println!("{phase}: {done}/{total}");
                    } else {
                        println!("{phase}");
                    }
                }
                JobEvent::Finished(Ok(JobOutcome::ImportedD2rSounds(path, _))) => {
                    println!("Validated sound packs: {}", path.display());
                    return Ok(());
                }
                JobEvent::Finished(Err(error)) => return Err(error),
                JobEvent::Finished(Ok(_)) => {
                    return Err("Sound worker returned an unexpected result".into())
                }
            }
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

#[derive(Debug)]
struct LaunchPlan {
    game: PathBuf,
    args: Vec<OsString>,
}

fn launch_plan(
    settings: &Settings,
    legacy: &Launcher,
    ready: &AssetReadiness,
) -> Result<LaunchPlan, String> {
    if !ready.game.ready {
        return Err("Choose the game executable in Launch options".into());
    }
    let game = ready
        .game
        .path
        .clone()
        .ok_or("The game executable could not be resolved")?;
    if !ready.base.ready {
        return Err("Select your Diablo game data before playing".into());
    }
    // The D2R master switch only changes flags and mod enables, never files.
    let sound_pack = if settings.use_d2r {
        settings.sound_pack
    } else {
        SoundPack::Original
    };
    let selected_sound = match sound_pack {
        SoundPack::Original => None,
        SoundPack::D2rClassic => Some(&ready.d2r_sounds_classic),
        SoundPack::D2rRemastered => Some(&ready.d2r_sounds_remastered),
    };
    if selected_sound.is_some_and(|status| !status.ready) {
        return Err(
            "Import the selected D2R sound pack in Sounds, or choose Diablo (original)".into(),
        );
    }
    if settings.use_hd && settings.hd_dir.is_some() && !ready.hd.ready {
        return Err(
            "The selected HD pack needs attention; import it again or switch HD art off".into(),
        );
    }
    if settings.runtime == Runtime::Toxoid && settings.use_d2r && settings.d2r_dir.is_some() {
        if settings.use_models && !ready.d2r_mapping.ready {
            return Err(
                "D2R characters are incomplete; import them or switch characters off".into(),
            );
        }
        if settings.use_environment && !ready.d2r_environment.ready {
            return Err(
                "D2R world assets are incomplete; import them or switch world assets off".into(),
            );
        }
    }
    let mut args: Vec<OsString> = Vec::new();
    let mut push = |flag: &str, value: &OsStr| {
        args.push(flag.into());
        args.push(value.to_owned());
    };
    if settings.runtime == Runtime::Toxoid {
        push("--render-mode", OsStr::new(&settings.render_mode));
        push(
            "--hd",
            OsStr::new(if settings.use_hd && ready.hd.ready {
                "on"
            } else {
                "off"
            }),
        );
        push(
            "--window",
            OsStr::new(&format!(
                "{}x{}",
                settings.resolution[0], settings.resolution[1]
            )),
        );
        push("--ui-size", OsStr::new("1280x720"));
        push(
            "--rt",
            OsStr::new(if settings.rt {
                settings.rt_quality.as_str()
            } else {
                "off"
            }),
        );
        push(
            "--d2r-models",
            OsStr::new(
                if settings.use_d2r && settings.use_models && ready.d2r_mapping.ready {
                    "on"
                } else {
                    "off"
                },
            ),
        );
        push(
            "--d2r-env",
            OsStr::new(
                if settings.use_d2r && settings.use_environment && ready.d2r_environment.ready {
                    "on"
                } else {
                    "off"
                },
            ),
        );
        let d2r = ready
            .d2r_mapping
            .path
            .as_ref()
            .or(ready.d2r_environment.path.as_ref());
        if !settings.use_d2r {
            // The host finds the managed import by default; --d2r off makes it ignore all of it.
            push("--d2r", OsStr::new("off"));
            push("--ui-theme", OsStr::new("classic"));
        } else if let Some(dir) = d2r {
            push("--d2r-dir", dir.as_os_str());
        }
        if settings.use_hd {
            if let Some(dir) = ready.hd.path.as_ref().filter(|_| ready.hd.ready) {
                push("--hd-dir", dir.as_os_str());
            }
        }
        args.push("--".into());
    }
    args.extend([
        OsString::from("--diablo"),
        OsString::from("--remember-launch-options"),
    ]);
    if let Some(dir) = &ready.base.path {
        args.extend([OsString::from("--data-dir"), dir.as_os_str().to_owned()]);
    }
    if settings.runtime == Runtime::Classic && settings.use_hd {
        if let Some(dir) = ready.hd.path.as_ref().filter(|_| ready.hd.ready) {
            args.extend([
                OsString::from("--hd"),
                OsString::from("--hd-dir"),
                dir.as_os_str().to_owned(),
            ]);
        }
    }
    if settings.runtime == Runtime::Classic {
        args.extend([
            OsString::from("--set"),
            format!(
                "Graphics.HD Graphics={}",
                u8::from(settings.use_hd && ready.hd.ready)
            )
            .into(),
        ]);
    }
    // The Toxoid window has its own resolution; native panels/picking share the
    // fixed 1280x720 UI coordinate space, independent of display size.
    for arg in legacy.args() {
        if arg.starts_with("Graphics.Width=") || arg.starts_with("Graphics.Height=") {
            continue;
        }
        args.extend([OsString::from("--set"), arg.into()]);
    }
    // Native sound selection and actual enabled mods stay in sync, including
    // explicitly returning to original audio after using either imported pack.
    for value in [
        format!("Audio.Sound Pack={}", sound_pack.native_value()),
        "Mods.d2sounds=0".into(),
        format!(
            "Mods.d2r-sounds-classic={}",
            u8::from(sound_pack == SoundPack::D2rClassic)
        ),
        format!(
            "Mods.d2r-sounds-advanced={}",
            u8::from(sound_pack == SoundPack::D2rRemastered)
        ),
    ] {
        args.extend([OsString::from("--set"), value.into()]);
    }
    let [width, height] = if settings.runtime == Runtime::Toxoid {
        [1280, 720]
    } else {
        settings.resolution
    };
    for value in [
        format!("Graphics.Width={width}"),
        format!("Graphics.Height={height}"),
    ] {
        args.extend([OsString::from("--set"), value.into()]);
    }
    Ok(LaunchPlan { game, args })
}

struct DesktopLauncher {
    settings: Settings,
    legacy: Launcher,
    paths: CachePaths,
    settings_path: PathBuf,
    saved: String,
    ready: Option<AssetReadiness>,
    requested_paths: AssetPaths,
    job: Option<JobHandle>,
    queued_jobs: VecDeque<JobRequest>,
    phase: String,
    progress: Option<(u64, Option<u64>)>,
    logs: VecDeque<String>,
    notice: Option<(bool, String)>,
    refresh_pending: bool,
    discover: mpsc::Receiver<AssetCandidates>,
    page: usize,
    cancelling: bool,
    close_after_job: bool,
    confirm_clear: bool,
    #[cfg(feature = "trainer")]
    trainer: Option<PathBuf>,
}

impl DesktopLauncher {
    fn new(
        settings: Settings,
        paths: CachePaths,
        settings_path: PathBuf,
        notice: Option<String>,
        _trainer: Option<PathBuf>,
    ) -> Self {
        let (tx, discover) = mpsc::channel();
        std::thread::spawn(move || {
            let _ = tx.send(assets::detect());
        });
        let legacy = Launcher::from_text(&settings.legacy_options);
        let requested_paths = settings.paths();
        let mut result = Self {
            settings,
            legacy,
            paths,
            settings_path,
            saved: String::new(),
            ready: None,
            requested_paths,
            job: None,
            queued_jobs: VecDeque::new(),
            phase: String::new(),
            progress: None,
            logs: VecDeque::new(),
            notice: notice.map(|n| (true, n)),
            refresh_pending: false,
            discover,
            page: 0,
            cancelling: false,
            close_after_job: false,
            confirm_clear: false,
            #[cfg(feature = "trainer")]
            trainer: _trainer,
        };
        result.refresh();
        result
    }

    fn start(&mut self, request: JobRequest) {
        if self.job.is_some() {
            return;
        }
        self.cancelling = false;
        self.progress = None;
        self.phase = "Preparing…".into();
        self.job = Some(JobHandle::spawn(request));
    }

    fn refresh(&mut self) {
        if self.close_after_job {
            return;
        }
        self.ready = None;
        self.requested_paths = self.settings.paths();
        if self.job.is_some() {
            self.refresh_pending = true;
        } else {
            self.start(JobRequest::Inspect(self.settings.paths()));
        }
    }

    fn log(&mut self, text: String) {
        for line in text.lines() {
            if self.logs.len() == 300 {
                self.logs.pop_front();
            }
            self.logs.push_back(line.to_owned());
        }
    }

    fn poll(&mut self) {
        if let Some(found) = self
            .discover
            .try_recv()
            .ok()
            .filter(|_| !self.close_after_job)
        {
            if self.settings.data_dir.is_none() {
                self.settings.data_dir = found.data_dirs.into_iter().next();
            }
            if self.settings.hd_dir.is_none() {
                self.settings.hd_dir = found.hd_dirs.into_iter().next();
            }
            if self.settings.d2r_dir.is_none() {
                self.settings.d2r_dir = found.d2r_dirs.into_iter().next();
            }
            if self.settings.d2r_install.is_none() {
                self.settings.d2r_install = found.d2r_installs.into_iter().next();
            }
            if self.settings.importer.is_none() {
                self.settings.importer = found.importers.into_iter().next();
            }
            if !self.settings.python.is_absolute() {
                if let Some(python) = found.pythons.into_iter().next() {
                    self.settings.python = python;
                }
            }
            self.refresh();
        }
        let events = self.job.as_ref().map(JobHandle::poll).unwrap_or_default();
        let mut finished = false;
        for event in events {
            match event {
                JobEvent::Progress { phase, done, total } => {
                    self.phase = phase;
                    self.progress = Some((done, total));
                }
                JobEvent::Log(text) => self.log(text),
                JobEvent::Finished(result) => {
                    finished = true;
                    match result {
                        Ok(JobOutcome::Inspected(ready)) => {
                            // Normalize selected wrapper folders to the actual art roots.
                            // Discovery may have selected newer paths while this check ran.
                            if !self.refresh_pending {
                                if ready.hd.ready {
                                    self.settings.hd_dir = ready.hd.path.clone();
                                }
                                if let Some(path) = ready
                                    .d2r_mapping
                                    .path
                                    .as_ref()
                                    .or(ready.d2r_environment.path.as_ref())
                                {
                                    self.settings.d2r_dir = Some(path.clone());
                                }
                                self.ready = Some(ready);
                            }
                        }
                        Ok(JobOutcome::InstalledHd(path)) => {
                            self.settings.hd_dir = Some(path);
                            self.settings.use_hd = true;
                            self.notice = Some((
                                false,
                                if self.queued_jobs.is_empty() {
                                    "HD art imported into your library."
                                } else {
                                    "Base pack copied; importing the addition…"
                                }
                                .into(),
                            ));
                            self.refresh_pending = true;
                        }
                        Ok(JobOutcome::ImportedD2r(path, reused)) => {
                            self.settings.d2r_dir = Some(path);
                            self.settings.use_d2r = true;
                            self.settings.use_models = true;
                            self.settings.use_environment = true;
                            self.notice = Some((
                                false,
                                if reused {
                                    "D2R art is already imported and unchanged; nothing was converted."
                                } else {
                                    "D2R characters, world, interface and portal imported."
                                }
                                .into(),
                            ));
                            self.refresh_pending = true;
                        }
                        Ok(JobOutcome::ImportedD2rSounds(path, reused)) => {
                            self.log(format!("Sound library: {}", path.display()));
                            self.settings.use_d2r = true;
                            self.settings.sound_pack = SoundPack::D2rRemastered;
                            self.notice = Some((
                                false,
                                if reused {
                                    "D2R sounds are already imported and unchanged. Remastered selected."
                                } else {
                                    "D2R classic and remastered sounds imported. Remastered selected."
                                }
                                .into(),
                            ));
                            self.refresh_pending = true;
                        }
                        Ok(JobOutcome::ClearedD2r(removed)) => {
                            if self
                                .settings
                                .d2r_dir
                                .as_ref()
                                .is_some_and(|dir| dir.starts_with(&self.paths.d2r_cache))
                            {
                                self.settings.d2r_dir = None;
                            }
                            self.settings.sound_pack = SoundPack::Original;
                            self.notice = Some((
                                false,
                                if removed.is_empty() {
                                    "Nothing to clear; no D2R import was found.".into()
                                } else {
                                    format!(
                                        "Cleared the D2R import ({} folder{}). Your D2R installation, saves and other mods were not touched.",
                                        removed.len(),
                                        if removed.len() == 1 { "" } else { "s" }
                                    )
                                },
                            ));
                            self.refresh_pending = true;
                        }
                        Err(error) => {
                            self.queued_jobs.clear();
                            self.log(error.clone());
                            self.notice = Some((true, error));
                        }
                    }
                }
            }
        }
        if finished {
            self.job = None;
            self.progress = None;
            self.cancelling = false;
            if !self.close_after_job {
                if let Some(next) = self.queued_jobs.pop_front() {
                    self.start(next);
                } else if self.refresh_pending {
                    self.refresh_pending = false;
                    self.refresh();
                }
            }
        }
    }

    fn cancel_job(&mut self) {
        self.queued_jobs.clear();
        if let Some(job) = &self.job {
            job.cancel();
            self.cancelling = true;
        }
    }

    fn choose_base(&mut self) {
        if let Some(file) = rfd::FileDialog::new()
            .set_title("Select DIABDAT.MPQ or SPAWN.MPQ")
            .add_filter("Diablo data", &["mpq", "MPQ"])
            .pick_file()
        {
            self.settings.data_dir = file.parent().map(Path::to_path_buf);
            self.refresh();
        }
    }

    fn choose_hd(&mut self) {
        if let Some(folder) = rfd::FileDialog::new()
            .set_title("Select an HD pack folder")
            .pick_folder()
        {
            self.settings.hd_dir = Some(folder);
            self.refresh();
        }
    }

    fn import_hd(&mut self, folder: bool) {
        let source = if folder {
            rfd::FileDialog::new()
                .set_title("Import an HD pack folder")
                .pick_folder()
        } else {
            rfd::FileDialog::new()
                .set_title("Import HD pack or 3D add-on ZIP")
                .add_filter("HD pack ZIP", &["zip"])
                .pick_file()
        };
        if let Some(source) = source {
            self.notice = None;
            let addition = JobRequest::InstallHd {
                source,
                destination: self.paths.hd_dir.clone(),
                overlay: true,
            };
            let external = self
                .ready
                .as_ref()
                .filter(|r| r.hd.ready)
                .and_then(|r| r.hd.path.as_ref())
                .filter(|p| *p != &self.paths.hd_dir)
                .cloned();
            if let Some(base) = external {
                self.queued_jobs.push_back(addition);
                self.start(JobRequest::CopyHdReference {
                    source: base,
                    destination: self.paths.hd_dir.clone(),
                });
            } else {
                self.start(addition);
            }
        }
    }

    fn import_d2r(&mut self) {
        let game = self
            .settings
            .d2r_install
            .clone()
            .filter(|p| assets::is_d2r_install(p))
            .or_else(|| {
                rfd::FileDialog::new()
                    .set_title("Select installed Diablo II Resurrected (.build.info + Data)")
                    .pick_folder()
            });
        let Some(game) = game else {
            return;
        };
        self.settings.d2r_install = Some(game.clone());
        let Some(importer) = self.settings.importer.clone() else {
            self.page = 1;
            self.notice = Some((
                true,
                "Locate the D2R importer tools in Launch options before importing.".into(),
            ));
            return;
        };
        self.notice = None;
        self.start(JobRequest::ImportD2r(D2rImportRequest {
            game_dir: game,
            cache_dir: self.paths.d2r_cache.clone(),
            importer_dir: importer,
            python: self.settings.python.clone(),
            create_venv: self.settings.managed_python,
        }));
    }

    fn import_d2r_sounds(&mut self) {
        let source = self
            .settings
            .d2r_install
            .clone()
            .filter(|p| assets::is_d2r_install(p))
            .or_else(|| {
                rfd::FileDialog::new()
                    .set_title("Select your installed Diablo II Resurrected")
                    .pick_folder()
            });
        let Some(source) = source else {
            return;
        };
        match sound_import_request(&self.settings, &self.paths, source.clone()) {
            Ok(request) => {
                self.settings.d2r_install = Some(source);
                self.notice = None;
                self.start(JobRequest::ImportD2rSounds(request));
            }
            Err(error) => self.notice = Some((true, error)),
        }
    }

    fn clear_d2r(&mut self) {
        // Never inside the owned install, the game data or the HD library.
        let protected = [
            self.settings.d2r_install.clone(),
            self.settings.data_dir.clone(),
            Some(self.paths.hd_dir.clone()),
        ]
        .into_iter()
        .flatten()
        .collect();
        self.notice = None;
        self.start(JobRequest::ClearD2r {
            cache_dir: self.paths.d2r_cache.clone(),
            sound_mods: self.paths.sound_mods.clone(),
            protected,
        });
    }

    fn persist(&mut self) {
        self.settings.legacy_options = self.legacy.text();
        let Ok(text) = serde_json::to_string_pretty(&self.settings) else {
            return;
        };
        if text != self.saved {
            match write_settings(&self.settings_path, &text) {
                Ok(()) => self.saved = text,
                Err(e) => {
                    self.notice = Some((true, format!("Could not save launcher choices: {e}")))
                }
            }
        }
    }

    fn play(&mut self, ctx: &egui::Context) {
        if self.settings.paths() != self.requested_paths {
            self.refresh();
            return;
        }
        let Some(ready) = &self.ready else {
            return;
        };
        let plan = match launch_plan(&self.settings, &self.legacy, ready) {
            Ok(p) => p,
            Err(e) => {
                self.notice = Some((true, e));
                return;
            }
        };
        self.persist();
        let mut command = std::process::Command::new(&plan.game);
        command.args(&plan.args);
        if self.settings.runtime == Runtime::Toxoid {
            if let Some(parent) = plan.game.parent() {
                for candidate in [
                    parent.join("assets"),
                    parent.join("../Resources/assets"),
                    parent.join("../Resources"),
                ] {
                    if candidate.join("txtdata/spells/spelldat.tsv").is_file() {
                        command.arg("--host-assets-dir").arg(candidate);
                        break;
                    }
                }
            }
        }
        if let Some(parent) = plan.game.parent() {
            command.current_dir(parent);
        }
        match command.spawn() {
            Ok(_) => {
                if self.settings.runtime == Runtime::Classic {
                    crate::register_url_scheme(&plan.game);
                }
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
            Err(e) => {
                self.notice = Some((
                    true,
                    format!("Could not start {}: {e}", plan.game.display()),
                ))
            }
        }
    }

    fn library(&mut self, ui: &mut egui::Ui) {
        let ready = self.ready.clone().unwrap_or_default();
        let idle = self.job.is_none();
        card(ui, "01", "Diablo game data", &ready.base, |ui| {
            ui.label("Select your Diablo archive. HD and D2R art are optional additions.");
            path_label(ui, self.settings.data_dir.as_deref());
            if ui
                .add_enabled(idle, egui::Button::new("Choose game data…"))
                .clicked()
            {
                self.choose_base();
            }
        });
        ui.add_space(10.0);
        card(ui, "02", "HD art pack", &ready.hd, |ui| {
            ui.label("HD art is optional and supplied separately by the DamnationX team. You can play with classic art or import D2R without this pack.");
            ui.label("Use an existing pack, or import a ZIP/folder into your library. Further imports add to the managed pack.");
            path_label(ui, self.settings.hd_dir.as_deref());
            ui.horizontal_wrapped(|ui| {
                if ui
                    .add_enabled(idle, egui::Button::new("Use folder…"))
                    .clicked()
                {
                    self.choose_hd();
                }
                if ui
                    .add_enabled(idle, egui::Button::new("Import ZIP…"))
                    .clicked()
                {
                    self.import_hd(false);
                }
                if ui
                    .add_enabled(idle, egui::Button::new("Import folder…"))
                    .clicked()
                {
                    self.import_hd(true);
                }
                ui.add_enabled(
                    idle && self.settings.hd_dir.is_some(),
                    egui::Checkbox::new(&mut self.settings.use_hd, "Use HD art"),
                );
            });
        });
        ui.add_space(10.0);
        let d2r = AssetStatus {
            ready: ready.d2r_ready(),
            path: self.settings.d2r_dir.clone(),
            detail: if self.settings.d2r_dir.is_some() {
                "Characters, world, interface and animated portal".into()
            } else {
                "Import from your own D2R installation, or select a converted cache".into()
            },
        };
        card(ui, "03", "Diablo II: Resurrected", &d2r, |ui| {
            ui.label("Bring D2R characters, scenery and interface into Tristram.");
            ui.small("Imports need Python 3.11+. Without a prebuilt d2rtool helper, they also need CMake 3.20+ and a C/C++17 compiler. Python can be selected under Launch options.");
            path_label(ui, self.settings.d2r_dir.as_deref());
            ui.horizontal_wrapped(|ui| {
                if ui
                    .add_enabled(idle, egui::Button::new("Import installed D2R…"))
                    .clicked()
                {
                    self.import_d2r();
                }
                if ui
                    .add_enabled(idle, egui::Button::new("Use converted assets…"))
                    .clicked()
                {
                    if let Some(path) = rfd::FileDialog::new()
                        .set_title("Select converted D2R cache or its glb folder")
                        .pick_folder()
                    {
                        self.settings.d2r_dir = Some(path);
                        self.refresh();
                    }
                }
                if ui.button("D2R sounds…").clicked() {
                    self.page = 2;
                }
            });
            let imported = ready.d2r_mapping.ready
                || ready.d2r_environment.ready
                || ready.d2r_ui.ready
                || ready.d2r_sounds_classic.ready
                || ready.d2r_sounds_remastered.ready;
            ui.add_space(4.0);
            ui.add_enabled(
                idle && imported,
                egui::Checkbox::new(
                    &mut self.settings.use_d2r,
                    egui::RichText::new("Use Diablo II: Resurrected assets").color(IVORY),
                ),
            )
            .on_hover_text("Switch between D2R and original art, interface and sounds instantly. Nothing is re-imported.")
            .on_disabled_hover_text("Import from your D2R installation first.");
            if !imported {
                ui.label(
                    egui::RichText::new("Nothing imported yet. Imports are cached: repeating one only converts what is missing or changed.")
                        .small()
                        .color(MUTED),
                );
            } else if !self.settings.use_d2r {
                ui.label(
                    egui::RichText::new(
                        "Original Diablo art, interface and sounds. Your import stays cached.",
                    )
                    .small()
                    .color(MUTED),
                );
            }
            if self.settings.d2r_dir.is_some() {
                ui.horizontal_wrapped(|ui| {
                    ui.add_enabled(
                        idle && self.settings.use_d2r,
                        egui::Checkbox::new(&mut self.settings.use_models, "Characters"),
                    );
                    ui.add_enabled(
                        idle && self.settings.use_d2r,
                        egui::Checkbox::new(&mut self.settings.use_environment, "World"),
                    );
                    for (label, status) in [
                        ("Characters", &ready.d2r_mapping),
                        ("World", &ready.d2r_environment),
                        ("Interface", &ready.d2r_ui),
                        ("Portal", &ready.d2r_portal),
                    ] {
                        ui.label(
                            egui::RichText::new(format!(
                                "{label}: {}",
                                if status.ready { "ready" } else { "missing" }
                            ))
                            .small()
                            .color(if status.ready {
                                GREEN
                            } else {
                                MUTED
                            }),
                        )
                        .on_hover_text(&status.detail);
                    }
                });
            }
            let managed = self.paths.d2r_cache.exists()
                || assets::D2R_SOUND_PACKS
                    .iter()
                    .any(|name| self.paths.sound_mods.join(name).exists());
            ui.add_space(4.0);
            if self.confirm_clear && idle {
                ui.horizontal_wrapped(|ui| {
                    ui.label(
                        egui::RichText::new("Delete the imported D2R art and sounds? You can import again afterwards.")
                            .color(ERROR),
                    );
                    if ui.button("Delete import").clicked() {
                        self.confirm_clear = false;
                        self.clear_d2r();
                    }
                    if ui.button("Cancel").clicked() {
                        self.confirm_clear = false;
                    }
                });
            } else if ui
                .add_enabled(idle && managed, egui::Button::new("Clear D2R import…"))
                .on_hover_text(format!(
                    "Removes only what the launcher imported:\n{}\n{}\nNever your D2R installation, saves, other mods or a converted folder you selected yourself.",
                    self.paths.d2r_cache.display(),
                    assets::D2R_SOUND_PACKS
                        .map(|name| self.paths.sound_mods.join(name).display().to_string())
                        .join("\n")
                ))
                .on_disabled_hover_text("No launcher-imported D2R assets were found.")
                .clicked()
            {
                self.confirm_clear = true;
            }
        });
        ui.add_space(10.0);
        ui.horizontal(|ui| {
            if ui
                .add_enabled(idle, egui::Button::new("Refresh asset checks"))
                .clicked()
            {
                self.refresh();
            }
            ui.weak("Artwork stays in your user library.");
        });
    }

    fn sounds(&mut self, ui: &mut egui::Ui) {
        let idle = self.job.is_none();
        let ready = self.ready.clone().unwrap_or_default();
        ui.heading("Diablo II: Resurrected sounds");
        ui.label("Bring the sounds of your own D2R installation into DamnationX.");
        if !self.settings.use_d2r {
            ui.label(
                egui::RichText::new(
                    "D2R assets are switched off in Library; original Diablo sounds will play.",
                )
                .color(GOLD),
            );
        }
        ui.add_space(12.0);
        let status = AssetStatus {
            ready: ready.d2r_sounds_classic.ready && ready.d2r_sounds_remastered.ready,
            path: Some(self.paths.sound_mods.clone()),
            detail: "Classic and remastered audio are imported separately from artwork.".into(),
        };
        card(ui, "01", "Import owned D2R sounds", &status, |ui| {
            ui.label("Imports both Classic and Remastered packs. Original Diablo audio remains available.");
            if let Some(path) = &self.settings.d2r_install {
                path_label(ui, Some(path));
            }
            ui.horizontal_wrapped(|ui| {
                if ui
                    .add_enabled(idle, egui::Button::new("Import D2R sounds…"))
                    .clicked()
                {
                    self.import_d2r_sounds();
                }
                if ui
                    .add_enabled(idle, egui::Button::new("Choose D2R installation…"))
                    .clicked()
                {
                    if let Some(path) = rfd::FileDialog::new()
                        .set_title("Select your installed Diablo II Resurrected")
                        .pick_folder()
                    {
                        self.settings.d2r_install = Some(path);
                    }
                }
                if ui
                    .add_enabled(idle, egui::Button::new("Refresh checks"))
                    .clicked()
                {
                    self.refresh();
                }
            });
            for (label, state) in [
                ("D2R Classic", &ready.d2r_sounds_classic),
                ("D2R Remastered", &ready.d2r_sounds_remastered),
            ] {
                ui.horizontal_wrapped(|ui| {
                    ui.label(egui::RichText::new(label).strong().color(IVORY));
                    ui.label(
                        egui::RichText::new(if state.ready {
                            &state.detail
                        } else {
                            "Not imported"
                        })
                        .color(if state.ready { GREEN } else { MUTED }),
                    )
                    .on_hover_text(&state.detail);
                });
            }
        });
        ui.add_space(16.0);
        egui::Frame::new()
            .fill(egui::Color32::from_rgb(23, 24, 27))
            .inner_margin(20.0)
            .corner_radius(5)
            .show(ui, |ui| {
                ui.label(
                    egui::RichText::new("YOUR SOUNDS. YOUR DESCENT.")
                        .color(GOLD)
                        .strong(),
                );
                ui.add_space(10.0);
                ui.horizontal_wrapped(|ui| {
                    ui.label("Sound pack");
                    ui.add_enabled_ui(idle, |ui| {
                        egui::ComboBox::from_id_salt("sound_pack")
                            .selected_text(self.settings.sound_pack.label())
                            .width(260.0)
                            .show_ui(ui, |ui| {
                                for (pack, available) in [
                                    (SoundPack::Original, true),
                                    (SoundPack::D2rClassic, ready.d2r_sounds_classic.ready),
                                    (SoundPack::D2rRemastered, ready.d2r_sounds_remastered.ready),
                                ] {
                                    ui.add_enabled_ui(available, |ui| {
                                        ui.selectable_value(
                                            &mut self.settings.sound_pack,
                                            pack,
                                            pack.label(),
                                        );
                                    });
                                }
                            });
                    });
                    ui.label(
                        egui::RichText::new(
                            if self.settings.sound_pack == SoundPack::D2rRemastered {
                                "REMASTERED SELECTED"
                            } else {
                                "YOUR CHOICE"
                            },
                        )
                        .small()
                        .color(GREEN),
                    );
                });
                ui.add_space(8.0);
                ui.label(
                    "Use original Diablo, D2R Classic, or D2R Remastered sounds when you play.",
                );
            });
        ui.add_space(14.0);
        ui.weak("Imported locally from your installed game. Sound import does not require Python or graphics conversion.");
    }

    fn options(&mut self, ui: &mut egui::Ui) {
        let idle = self.job.is_none();
        ui.add_enabled_ui(idle, |ui| {
            ui.heading("Game & display");
            ui.horizontal(|ui| {
                ui.label("Game runtime");
                egui::ComboBox::from_id_salt("runtime").selected_text(if self.settings.runtime==Runtime::Toxoid {"3D game (Toxoid)"} else {"Classic SDL game"}).show_ui(ui, |ui| {
                    ui.selectable_value(&mut self.settings.runtime,Runtime::Toxoid,"3D game (Toxoid)");
                    ui.selectable_value(&mut self.settings.runtime,Runtime::Classic,"Classic SDL game");
                });
            });
            ui.horizontal(|ui| {
                ui.label("Game executable"); path_edit(ui,&mut self.settings.game);
                if ui.button("Browse…").clicked() {
                    if let Some(game) = rfd::FileDialog::new().set_title("Select damnationx_host or the classic game executable").pick_file() {
                        let game = game_executable(&game); self.settings.runtime=runtime_for(&game); self.settings.game=game;
                    }
                }
            });
            ui.horizontal(|ui| {
                ui.label("Resolution");
                egui::ComboBox::from_id_salt("native_resolution").selected_text(format!("{} × {}",self.settings.resolution[0],self.settings.resolution[1])).show_ui(ui, |ui| {
                    for &(w,h) in RESOLUTIONS { if ui.selectable_label(self.settings.resolution==[w,h],format!("{w} × {h}")).clicked() { self.settings.resolution=[w,h]; self.settings.auto_resolution=false; } }
                });
                ui.checkbox(&mut self.settings.auto_resolution,"Match display");
            });
            if self.settings.runtime==Runtime::Toxoid {
                ui.horizontal(|ui| {
                    ui.label("Renderer");
                    egui::ComboBox::from_id_salt("render_mode").selected_text(match self.settings.render_mode.as_str() {"classic"=>"Classic", "hd"=>"HD 2D", _=>"3D Resurrect Update"}).show_ui(ui, |ui| {
                        for (value,label) in [("3d","3D Resurrect Update"),("classic","Classic"),("hd","HD 2D")] { ui.selectable_value(&mut self.settings.render_mode,value.into(),label); }
                    });
                    ui.checkbox(&mut self.settings.rt,"Ray tracing").on_hover_text("Optional on supported GPUs. Disabled by default.");
                    ui.add_enabled_ui(self.settings.rt, |ui| {
                        egui::ComboBox::from_id_salt("rt_quality").selected_text(&self.settings.rt_quality).show_ui(ui, |ui| {
                            for tier in ["low","medium","high","ultra"] { ui.selectable_value(&mut self.settings.rt_quality,tier.into(),tier); }
                        });
                    });
                });
            }
            ui.add_space(10.0); ui.separator(); ui.heading("Gameplay");
            for (i,(key,label,_)) in TOGGLES.iter().enumerate().filter(|(_, (key, ..))| key.starts_with("Game.") || key.starts_with("Mods.")) {
                let _=key; ui.checkbox(&mut self.legacy.on[i],*label);
            }
            if self.settings.runtime==Runtime::Classic {
                ui.separator(); ui.heading("Classic display");
                for (i,(_,label,_)) in TOGGLES.iter().enumerate().filter(|(_, (key, ..))| key.starts_with("Graphics.")) { ui.checkbox(&mut self.legacy.on[i],*label); }
            }
            ui.add_space(10.0); ui.separator();
            egui::CollapsingHeader::new("Import tools & library locations").show(ui, |ui| {
                ui.label("D2R conversion uses Python 3.11+ and the included importer. A managed environment installs its Python dependencies.");
                ui.horizontal(|ui| { ui.label("Python"); path_edit(ui,&mut self.settings.python); if ui.button("Browse interpreter…").clicked() { if let Some(path)=rfd::FileDialog::new().set_title("Select Python 3.11+ executable").pick_file() {self.settings.python=path;} } });
                ui.checkbox(&mut self.settings.managed_python,"Use a managed Python environment (recommended)");
                optional_path_picker(ui,"Importer tools",&mut self.settings.importer);
                optional_path_picker(ui,"Installed D2R",&mut self.settings.d2r_install);
                ui.weak(format!("HD library: {}",self.paths.hd_dir.display()));
                ui.weak(format!("D2R library: {}",self.paths.d2r_cache.display()));
                ui.weak(format!("Settings: {}",self.settings_path.display()));
            });
            #[cfg(feature="trainer")]
            if let Some(trainer)=&self.trainer { if ui.button("Developer trainer").clicked() { if let Err(e)=std::process::Command::new(trainer).spawn() {self.notice=Some((true,e.to_string()));} } }
        });
    }
}

fn path_label(ui: &mut egui::Ui, path: Option<&Path>) {
    ui.label(
        egui::RichText::new(
            path.map(|p| p.display().to_string())
                .unwrap_or_else(|| "No folder selected".into()),
        )
        .small()
        .color(MUTED),
    )
    .on_hover_text(path.map(|p| p.display().to_string()).unwrap_or_default());
}

fn path_edit(ui: &mut egui::Ui, path: &mut PathBuf) {
    let mut value = path.to_string_lossy().into_owned();
    if ui
        .add(egui::TextEdit::singleline(&mut value).desired_width(300.0))
        .changed()
    {
        *path = value.into();
    }
}

fn optional_path_picker(ui: &mut egui::Ui, label: &str, path: &mut Option<PathBuf>) {
    ui.horizontal(|ui| {
        ui.label(label);
        path_label(ui, path.as_deref());
        if ui.button("Browse…").clicked() {
            if let Some(folder) = rfd::FileDialog::new().set_title(label).pick_folder() {
                *path = Some(folder);
            }
        }
    });
}

fn card(
    ui: &mut egui::Ui,
    index: &str,
    title: &str,
    status: &AssetStatus,
    contents: impl FnOnce(&mut egui::Ui),
) {
    egui::Frame::new()
        .fill(egui::Color32::from_rgb(23, 24, 27))
        .stroke(egui::Stroke::new(1.0, egui::Color32::from_rgb(72, 64, 46)))
        .inner_margin(18.0)
        .corner_radius(5)
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                ui.label(egui::RichText::new(index).color(GOLD).strong());
                ui.label(egui::RichText::new(title).size(20.0).color(IVORY));
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label(
                        egui::RichText::new(if status.ready { "READY" } else { "NOT SET UP" })
                            .small()
                            .color(if status.ready { GREEN } else { MUTED }),
                    )
                    .on_hover_text(&status.detail);
                });
            });
            ui.add_space(8.0);
            contents(ui);
            if !status.ready && status.path.is_some() {
                ui.label(egui::RichText::new(&status.detail).small().color(GOLD));
            }
        });
}

fn style(ctx: &egui::Context) {
    ctx.set_theme(egui::Theme::Dark);
    let mut style = (*ctx.style_of(egui::Theme::Dark)).clone();
    style.visuals = egui::Visuals::dark();
    style.visuals.override_text_color = Some(IVORY);
    style.visuals.panel_fill = egui::Color32::from_rgb(12, 14, 18);
    style.visuals.window_fill = egui::Color32::from_rgb(23, 24, 27);
    style.visuals.faint_bg_color = egui::Color32::from_rgb(30, 30, 29);
    style.visuals.selection.bg_fill = egui::Color32::from_rgb(83, 67, 38);
    style.visuals.widgets.inactive.bg_fill = egui::Color32::from_rgb(35, 34, 32);
    style.visuals.widgets.inactive.bg_stroke =
        egui::Stroke::new(1.0, egui::Color32::from_rgb(91, 77, 49));
    style.visuals.widgets.hovered.bg_fill = egui::Color32::from_rgb(61, 50, 33);
    style.visuals.widgets.hovered.bg_stroke = egui::Stroke::new(1.0, GOLD);
    style.visuals.widgets.active.bg_fill = egui::Color32::from_rgb(79, 61, 33);
    style.spacing.item_spacing = egui::vec2(10.0, 8.0);
    style.spacing.button_padding = egui::vec2(13.0, 8.0);
    style
        .text_styles
        .insert(egui::TextStyle::Body, egui::FontId::proportional(15.0));
    style
        .text_styles
        .insert(egui::TextStyle::Button, egui::FontId::proportional(15.0));
    ctx.set_style_of(egui::Theme::Dark, style);
}

impl eframe::App for DesktopLauncher {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        if ui.ctx().input(|i| i.viewport().close_requested()) && self.job.is_some() {
            ui.ctx()
                .send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.close_after_job = true;
            self.cancel_job();
        }
        self.poll();
        if self.close_after_job && self.job.is_none() {
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
        }
        if let Some(job) = &self.job {
            ui.ctx().request_repaint_after(if job.is_finished() {
                Duration::ZERO
            } else {
                Duration::from_millis(80)
            });
        } else {
            ui.ctx().request_repaint_after(Duration::from_millis(500));
        }
        if self.settings.auto_resolution {
            if let Some(size) = ui.ctx().input(|i| {
                i.viewport()
                    .monitor_size
                    .map(|s| s * i.viewport().native_pixels_per_point.unwrap_or(1.0))
            }) {
                self.settings.resolution = [
                    (size.x as u32).clamp(640, 7680),
                    (size.y as u32).clamp(480, 4320),
                ];
            }
        }
        egui::CentralPanel::default().show(ui,|ui| {
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                ui.vertical(|ui| {
                    ui.label(egui::RichText::new("DAMNATIONX").size(33.0).strong().color(GOLD));
                    ui.label(egui::RichText::new("PREPARE YOUR DESCENT").small().color(MUTED));
                });
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center),|ui| {
                    ui.selectable_value(&mut self.page,2,"Sounds"); ui.selectable_value(&mut self.page,1,"Launch options"); ui.selectable_value(&mut self.page,0,"Asset library");
                });
            });
            ui.add_space(12.0);ui.separator();ui.add_space(8.0);
            egui::Panel::bottom("playbar").frame(egui::Frame::new().inner_margin(12.0)).show(ui,|ui| {
                if let Some((error,text))=&self.notice {ui.label(egui::RichText::new(text).color(if *error {ERROR} else {GREEN}));ui.add_space(6.0);}
                if self.job.is_some() {
                    ui.horizontal(|ui| {
                        ui.spinner();ui.label(if self.cancelling {"Stopping and preserving existing assets…"} else {&self.phase});
                        if ui.add_enabled(!self.cancelling,egui::Button::new("Cancel")).clicked() {self.cancel_job();}
                    });
                    if let Some((done,Some(total)))=self.progress.filter(|(_,t)|t.is_some_and(|v|v>0)) {
                        ui.add(egui::ProgressBar::new((done as f64/total as f64).clamp(0.0,1.0) as f32).text(format!("{done} / {total}")));
                    }
                }
                let plan=self.ready.as_ref().map(|r|launch_plan(&self.settings,&self.legacy,r));
                let can_play=self.job.is_none() && plan.as_ref().is_some_and(|r|r.is_ok());
                ui.horizontal(|ui| {
                    let response=ui.add_enabled(can_play,egui::Button::new(egui::RichText::new("PLAY").size(22.0).strong().color(GOLD)).min_size(egui::vec2(200.0,48.0)));
                    if response.clicked() {let ctx=ui.ctx().clone();self.play(&ctx);}
                    ui.vertical(|ui| {
                        ui.label(if can_play {"Ready to enter Tristram"} else if self.job.is_some() {"Finish or cancel setup before playing"} else {"Setup needs attention"});
                        if let Some(Err(error))=plan {ui.label(egui::RichText::new(error).small().color(MUTED));}
                    });
                });
            });
            egui::ScrollArea::vertical().auto_shrink([false,false]).show(ui,|ui| {
                match self.page { 0 => self.library(ui), 2 => self.sounds(ui), _ => self.options(ui) }
                if !self.logs.is_empty() {
                    ui.add_space(12.0);
                    egui::CollapsingHeader::new("Import activity").default_open(self.job.is_some()).show(ui,|ui| {
                        egui::ScrollArea::vertical().id_salt("import_logs").max_height(160.0).stick_to_bottom(true).show(ui,|ui| {
                            for line in &self.logs {ui.monospace(line);}
                        });
                    });
                }
                ui.add_space(10.0);ui.label(egui::RichText::new("Use files from your own Diablo and Diablo II: Resurrected installations.").small().color(MUTED));
            });
        });
        if self.settings.paths() != self.requested_paths {
            self.refresh();
        }
        self.persist();
    }

    fn on_exit(&mut self, _gl: Option<&eframe::glow::Context>) {
        if let Some(job) = &self.job {
            job.cancel();
        }
        self.persist();
    }
}

pub fn run() -> eframe::Result {
    let args = match cli(std::env::args_os().skip(1)) {
        Ok(args) => args,
        Err(e) => {
            eprintln!("{e}\nUse --help for launcher options.");
            std::process::exit(2);
        }
    };
    if args.help {
        println!("DamnationX Launcher\n\nlauncher [path/to/game] [--game PATH] [--settings FILE] [--page library|options|sounds]\nlauncher --game PATH --import-d2r-sounds OWNED_D2R_FOLDER [--settings FILE]\n\nImports owned HD packs, D2R art and D2R sound packs before starting the game.\nThe sound import command opens no window and selects Remastered only after validation.\nWindows, macOS and Linux desktop; the browser frontend uses its File Manager.");
        return Ok(());
    }
    let paths = match CachePaths::for_user() {
        Ok(paths) => paths,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(2);
        }
    };
    let settings_path = args
        .settings
        .unwrap_or_else(|| paths.config_dir.join("settings.json"));
    let old = std::env::current_exe()
        .ok()
        .map(|p| p.with_file_name("launcher-settings.txt"));
    let (mut settings, notice) = load_settings(&settings_path, old.as_deref());
    if let Some(game) = args.game.or_else(|| {
        settings
            .game
            .as_os_str()
            .is_empty()
            .then(nearby_game)
            .flatten()
    }) {
        let game = game_executable(&game);
        settings.runtime = runtime_for(&game);
        settings.game = game;
    }
    if let Some(source) = args.import_d2r_sounds {
        let result = sound_import_request(&settings, &paths, source)
            .and_then(import_sounds_without_window)
            .and_then(|()| write_sound_selection(&settings_path, &settings));
        match result {
            Ok(()) => {
                println!("D2R Remastered selected. Other settings and saves preserved.");
                return Ok(());
            }
            Err(error) => {
                eprintln!("{error}");
                std::process::exit(1);
            }
        }
    }
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([980.0, 870.0])
            .with_min_inner_size([760.0, 600.0]),
        ..Default::default()
    };
    eframe::run_native(
        "DamnationX Launcher",
        options,
        Box::new(move |cc| {
            style(&cc.egui_ctx);
            let mut launcher =
                DesktopLauncher::new(settings, paths, settings_path, notice, args.trainer);
            launcher.page = args.page;
            Ok(Box::new(launcher))
        }),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ready(settings: &Settings, path: &Path) -> AssetReadiness {
        let status = AssetStatus {
            ready: true,
            path: Some(path.to_path_buf()),
            detail: "fixture".into(),
        };
        AssetReadiness {
            game: AssetStatus {
                ready: true,
                path: Some(settings.game.canonicalize().unwrap()),
                detail: "fixture executable".into(),
            },
            base: status.clone(),
            hd: status.clone(),
            d2r_mapping: status.clone(),
            d2r_environment: status.clone(),
            d2r_ui: status.clone(),
            d2r_portal: status.clone(),
            d2r_sounds_classic: status.clone(),
            d2r_sounds_remastered: status,
        }
    }

    #[test]
    fn host_args_keep_native_flags_after_separator_and_default_rt_off() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game executable");
        std::fs::write(&game, b"fixture").unwrap();
        let settings = Settings {
            game: game.clone(),
            data_dir: Some(temp.path().into()),
            hd_dir: Some(temp.path().into()),
            d2r_dir: Some(temp.path().into()),
            resolution: [2560, 1440],
            ..Default::default()
        };
        let plan = launch_plan(
            &settings,
            &Launcher::defaults(),
            &ready(&settings, temp.path()),
        )
        .unwrap();
        let args: Vec<_> = plan.args.iter().map(|s| s.to_str().unwrap()).collect();
        let split = args.iter().position(|s| *s == "--").unwrap();
        assert_eq!(&args[..4], ["--render-mode", "3d", "--hd", "on"]);
        assert!(args[..split]
            .windows(2)
            .any(|v| v == ["--window", "2560x1440"]));
        assert!(args[..split].windows(2).any(|v| v == ["--rt", "off"]));
        assert!(args[split + 1..].contains(&"--diablo"));
        assert!(!args[..split].contains(&"--set"));
        assert!(args[split + 1..].contains(&"Graphics.Width=1280"));
        assert!(!args[split + 1..].contains(&"Graphics.Width=2560"));
        assert_eq!(plan.game, game.canonicalize().unwrap());
    }

    #[test]
    fn d2r_switch_flips_art_interface_and_sounds_without_import() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("host");
        std::fs::write(&game, b"fixture").unwrap();
        let mut settings = Settings {
            game,
            data_dir: Some(temp.path().into()),
            d2r_dir: Some(temp.path().into()),
            sound_pack: SoundPack::D2rRemastered,
            ..Default::default()
        };
        let mut status = ready(&settings, temp.path());
        let args = |settings: &Settings, status: &AssetReadiness| -> Vec<String> {
            launch_plan(settings, &Launcher::defaults(), status)
                .unwrap()
                .args
                .iter()
                .map(|s| s.to_string_lossy().into_owned())
                .collect()
        };
        let has = |args: &[String], pair: [&str; 2]| args.windows(2).any(|w| w == pair);
        let on = args(&settings, &status);
        assert!(has(&on, ["--d2r-models", "on"]) && has(&on, ["--d2r-env", "on"]));
        assert!(on.iter().any(|a| a == "--d2r-dir"));
        assert!(on.iter().any(|a| a == "Mods.d2r-sounds-advanced=1"));
        settings.use_d2r = false;
        let off = args(&settings, &status);
        assert!(has(&off, ["--d2r-models", "off"]) && has(&off, ["--d2r-env", "off"]));
        assert!(has(&off, ["--ui-theme", "classic"]) && has(&off, ["--d2r", "off"]));
        assert!(!off.iter().any(|a| a == "--d2r-dir"));
        for value in [
            "Audio.Sound Pack=0",
            "Mods.d2r-sounds-classic=0",
            "Mods.d2r-sounds-advanced=0",
        ] {
            assert!(off.iter().any(|a| a == value), "{value}");
        }
        // Remembered sub-choices survive the switch.
        assert_eq!(settings.sound_pack, SoundPack::D2rRemastered);
        assert!(settings.use_models && settings.use_environment);
        // Switched off, missing or cleared imports never block Play.
        status.d2r_mapping.ready = false;
        status.d2r_environment.ready = false;
        status.d2r_sounds_remastered.ready = false;
        assert!(launch_plan(&settings, &Launcher::defaults(), &status).is_ok());
        settings.use_d2r = true;
        assert!(launch_plan(&settings, &Launcher::defaults(), &status).is_err());
        // Older settings files without the switch keep D2R on.
        let old: Settings = serde_json::from_str(r#"{"use_models": false}"#).unwrap();
        assert!(old.use_d2r && !old.use_models);
    }

    #[test]
    fn classic_arguments_do_not_receive_toxoid_options() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("devilutionx");
        std::fs::write(&game, b"fixture").unwrap();
        let settings = Settings {
            game,
            runtime: Runtime::Classic,
            resolution: [1920, 1080],
            ..Default::default()
        };
        let plan = launch_plan(
            &settings,
            &Launcher::defaults(),
            &ready(&settings, temp.path()),
        )
        .unwrap();
        assert!(!plan
            .args
            .iter()
            .any(|s| s == "--" || s == "--rt" || s == "--d2r-models"));
        assert!(plan.args.iter().any(|s| s == "Graphics.Width=1920"));
        assert!(plan.args.iter().any(|s| s == "Graphics.Height=1080"));
        assert!(!plan.args.iter().any(|s| s == "Graphics.Width=640"));
    }

    #[test]
    fn relative_game_paths_resolve_before_changing_child_directory() {
        let temp = tempfile::tempdir_in(".").unwrap();
        let game = PathBuf::from(".")
            .join(temp.path().file_name().unwrap())
            .join("game");
        std::fs::write(&game, b"fixture").unwrap();
        assert!(!game.is_absolute());
        let settings = Settings {
            game: game.clone(),
            ..Default::default()
        };
        let plan = launch_plan(
            &settings,
            &Launcher::defaults(),
            &ready(&settings, temp.path()),
        )
        .unwrap();
        assert_eq!(plan.game, game.canonicalize().unwrap());
        assert!(plan.game.is_absolute());
    }

    #[test]
    fn legacy_settings_migrate_display_size_and_auto_choice() {
        let temp = tempfile::tempdir().unwrap();
        let old = temp.path().join("launcher-settings.txt");
        std::fs::write(
            &old,
            "Graphics.Width=2560\nGraphics.Height=1440\nAutoResolution=0\nHdDir=old art",
        )
        .unwrap();
        let (settings, error) = load_settings(&temp.path().join("new.json"), Some(&old));
        assert!(error.is_none());
        assert_eq!(settings.resolution, [2560, 1440]);
        assert!(!settings.auto_resolution);
        assert_eq!(settings.hd_dir, Some(PathBuf::from("old art")));
    }

    #[test]
    fn invalid_selected_assets_block_launch_but_unselected_optional_assets_do_not() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("host");
        std::fs::write(&game, b"fixture").unwrap();
        let mut settings = Settings {
            game,
            hd_dir: Some(temp.path().into()),
            ..Default::default()
        };
        let mut status = ready(&settings, temp.path());
        status.hd.ready = false;
        assert!(launch_plan(&settings, &Launcher::defaults(), &status).is_err());
        settings.use_hd = false;
        assert!(launch_plan(&settings, &Launcher::defaults(), &status).is_ok());
        status.base.ready = false;
        assert!(launch_plan(&settings, &Launcher::defaults(), &status).is_err());
    }

    #[test]
    fn settings_round_trip_paths_and_corrupt_file_is_not_silently_accepted() {
        assert_eq!(
            serde_json::from_str::<Runtime>("\"Bevy\"").unwrap(),
            Runtime::Toxoid
        );
        assert_eq!(
            serde_json::to_string(&Runtime::Toxoid).unwrap(),
            "\"Toxoid\""
        );
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("settings.json");
        let settings = Settings {
            data_dir: Some(PathBuf::from("a path/ユニコード\nname")),
            ..Default::default()
        };
        write_settings(&path, &serde_json::to_string(&settings).unwrap()).unwrap();
        let (back, error) = load_settings(&path, None);
        assert!(error.is_none());
        assert_eq!(back.data_dir, settings.data_dir);
        std::fs::write(&path, b"{broken").unwrap();
        let (_, error) = load_settings(&path, None);
        assert!(error.is_some());
    }

    #[test]
    fn cli_preserves_argument_paths_and_rejects_missing_values() {
        let args =
            cli(["--game", "C:\\My games\\host.exe", "--settings", "a b.json"].map(OsString::from))
                .unwrap();
        assert_eq!(args.game, Some(PathBuf::from("C:\\My games\\host.exe")));
        assert_eq!(args.settings, Some(PathBuf::from("a b.json")));
        assert!(cli([OsString::from("--game")]).is_err());
        assert!(cli([OsString::from("--unknown")]).is_err());
        assert_eq!(
            cli(["--page", "options"].map(OsString::from)).unwrap().page,
            1
        );
        assert_eq!(
            cli(["--page", "library"].map(OsString::from)).unwrap().page,
            0
        );
        assert!(cli([OsString::from("--page")]).is_err());
        assert!(cli(["--page", "missing"].map(OsString::from)).is_err());
        assert_eq!(
            cli(["--page", "sounds"].map(OsString::from)).unwrap().page,
            2
        );
        assert_eq!(
            cli(["--import-d2r-sounds", "/owned game with spaces"].map(OsString::from))
                .unwrap()
                .import_d2r_sounds,
            Some(PathBuf::from("/owned game with spaces"))
        );
        assert!(cli([OsString::from("--import-d2r-sounds")]).is_err());
    }

    #[test]
    fn sound_selection_is_explicit_and_unavailable_packs_do_not_launch() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("host");
        std::fs::write(&game, b"fixture executable").unwrap();
        let mut settings = Settings {
            game,
            ..Default::default()
        };
        let mut status = ready(&settings, temp.path());
        for runtime in [Runtime::Toxoid, Runtime::Classic] {
            settings.runtime = runtime;
            for pack in [
                SoundPack::Original,
                SoundPack::D2rClassic,
                SoundPack::D2rRemastered,
            ] {
                settings.sound_pack = pack;
                let plan = launch_plan(&settings, &Launcher::defaults(), &status).unwrap();
                let args: Vec<_> = plan.args.iter().map(|s| s.to_str().unwrap()).collect();
                assert!(
                    args.contains(&format!("Audio.Sound Pack={}", pack.native_value()).as_str())
                );
                assert!(args.contains(
                    &format!(
                        "Mods.d2r-sounds-classic={}",
                        u8::from(pack == SoundPack::D2rClassic)
                    )
                    .as_str()
                ));
                assert!(args.contains(
                    &format!(
                        "Mods.d2r-sounds-advanced={}",
                        u8::from(pack == SoundPack::D2rRemastered)
                    )
                    .as_str()
                ));
                assert!(args.contains(&"Mods.d2sounds=0"));
                assert!(
                    !args.contains(&"--save-dir"),
                    "audio selection must preserve the save location"
                );
            }
        }
        status.d2r_sounds_remastered.ready = false;
        assert!(launch_plan(&settings, &Launcher::defaults(), &status)
            .unwrap_err()
            .contains("Import the selected"));
        settings.sound_pack = SoundPack::Original;
        assert!(launch_plan(&settings, &Launcher::defaults(), &status).is_ok());
    }

    #[test]
    fn successful_sound_import_changes_only_the_authorized_json_value() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("settings with spaces.json");
        let before = serde_json::json!({
            "runtime": "Bevy", "game": "", "resolution": [1920, 1080],
            "legacy_options": "existing\nsettings", "sound_pack": "original",
            "future_setting": { "keep": true }, "d2r_install": null,
        });
        std::fs::write(&path, serde_json::to_vec(&before).unwrap()).unwrap();
        write_sound_selection(&path, &Settings::default()).unwrap();
        let mut after: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        assert_eq!(after["sound_pack"], "d2r_remastered");
        after["sound_pack"] = "original".into();
        assert_eq!(after, before);
        let settings: Settings = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        assert_eq!(settings.sound_pack, SoundPack::D2rRemastered);
        let migrated: Settings = serde_json::from_str("{\"runtime\":\"Bevy\"}").unwrap();
        assert_eq!(migrated.sound_pack, SoundPack::Original);
        std::fs::write(&path, br#"{"use_d2r": false}"#).unwrap();
        write_sound_selection(&path, &Settings::default()).unwrap();
        let switched: Settings = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        assert!(switched.use_d2r);
        std::fs::write(&path, b"{malformed").unwrap();
        assert!(write_sound_selection(&path, &Settings::default()).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), b"{malformed");
    }

    #[test]
    fn portal_preference_keeps_both_native_switches_in_sync() {
        let state = Launcher::from_text("Mods.permanent_cathedral_portal=0");
        assert!(state.args().contains(&"Game.Cathedral Portal=0".into()));
        assert!(state
            .args()
            .contains(&"Mods.permanent_cathedral_portal=0".into()));
        assert!(Launcher::defaults()
            .args()
            .contains(&"Graphics.Ray Traced Lighting=0".into()));
    }
}
