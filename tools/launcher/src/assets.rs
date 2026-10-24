//! Native asset discovery and bounded manifest checks. Run `inspect` on a worker:
//! even a small filesystem query can block when a selected drive is unavailable.
use serde_json::Value;
use std::collections::BTreeSet;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Component, Path, PathBuf};

#[derive(Clone, Debug)]
pub struct CachePaths {
    pub config_dir: PathBuf,
    pub hd_dir: PathBuf,
    /// Importer cache parent; the host receives its `glb` child.
    pub d2r_cache: PathBuf,
    /// The native game's existing preference tree; saves and other mods stay put.
    pub sound_mods: PathBuf,
}

impl CachePaths {
    pub fn for_user() -> Result<Self, String> {
        let dirs =
            directories::BaseDirs::new().ok_or("Cannot locate the current user's folders")?;
        #[cfg(target_os = "macos")]
        let data = dirs
            .home_dir()
            .join("Library/Application Support/damnationx");
        #[cfg(not(target_os = "macos"))]
        let data = dirs.data_local_dir().join("damnationx");
        #[cfg(any(target_os = "macos", target_os = "windows"))]
        let d2r_cache = data.join("d2r-models");
        #[cfg(not(any(target_os = "macos", target_os = "windows")))]
        let d2r_cache = dirs.cache_dir().join("damnationx/d2r-models");
        #[cfg(target_os = "windows")]
        let config_dir = data.join("launcher");
        #[cfg(not(target_os = "windows"))]
        let config_dir = dirs.config_dir().join("damnationx/launcher");
        Ok(Self {
            config_dir,
            hd_dir: data.join("hd-combined/hd"),
            d2r_cache,
            sound_mods: dirs.data_dir().join("diasurgical/devilution/mods"),
        })
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AssetPaths {
    pub game: Option<PathBuf>,
    pub data_dir: Option<PathBuf>,
    pub hd_dir: Option<PathBuf>,
    pub d2r_dir: Option<PathBuf>,
    pub sound_mods: Option<PathBuf>,
}

#[derive(Clone, Debug, Default)]
pub struct AssetStatus {
    pub ready: bool,
    pub path: Option<PathBuf>,
    pub detail: String,
}

impl AssetStatus {
    fn result(path: Option<PathBuf>, result: Result<String, String>) -> Self {
        match result {
            Ok(detail) => Self {
                ready: true,
                path,
                detail,
            },
            Err(detail) => Self {
                ready: false,
                path,
                detail,
            },
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct AssetReadiness {
    pub game: AssetStatus,
    pub base: AssetStatus,
    pub hd: AssetStatus,
    pub d2r_mapping: AssetStatus,
    pub d2r_environment: AssetStatus,
    pub d2r_ui: AssetStatus,
    pub d2r_portal: AssetStatus,
    pub d2r_sounds_classic: AssetStatus,
    pub d2r_sounds_remastered: AssetStatus,
}

pub const D2R_SOUND_PACKS: [&str; 2] = ["d2r-sounds-classic", "d2r-sounds-advanced"];

/// Inspect the actual native pack, not a graphics cache or an import receipt.
/// A manifest is the native importer's completion marker; audio bytes must also
/// carry a supported container header, even though their filenames end in WAV.
pub fn sound_pack_status(root: &Path) -> Result<String, String> {
    let manifest = root.join("manifest.ini");
    regular_file(&manifest)?;
    if fs::metadata(&manifest).map_err(|e| e.to_string())?.len() > 64 * 1024 {
        return Err("Sound pack manifest exceeds 64 KiB".into());
    }
    let manifest_bytes = fs::read(&manifest).map_err(|e| e.to_string())?;
    if !String::from_utf8_lossy(&manifest_bytes).contains("[mod]") {
        return Err("Sound pack has no valid completed import manifest".into());
    }
    let mut stack = vec![(root.to_path_buf(), 0usize)];
    let mut samples = 0usize;
    let mut bytes = 0u64;
    let mut nodes = 0usize;
    while let Some((directory, depth)) = stack.pop() {
        if depth > 32 {
            return Err("Sound pack directory nesting exceeds 32 levels".into());
        }
        let meta = fs::symlink_metadata(&directory).map_err(|e| e.to_string())?;
        if meta.file_type().is_symlink() || !meta.is_dir() {
            return Err("Sound pack contains a directory link or special file".into());
        }
        for entry in fs::read_dir(&directory).map_err(|e| e.to_string())? {
            let path = entry.map_err(|e| e.to_string())?.path();
            nodes += 1;
            if nodes > 200_000 {
                return Err("Sound pack contains too many files".into());
            }
            let meta = fs::symlink_metadata(&path).map_err(|e| e.to_string())?;
            if meta.file_type().is_symlink() {
                return Err("Sound pack contains a file link".into());
            }
            relative_path(
                &path
                    .strip_prefix(root)
                    .map_err(|e| e.to_string())?
                    .to_string_lossy(),
            )?;
            if meta.is_dir() {
                stack.push((path, depth + 1));
            } else if !meta.is_file() {
                return Err("Sound pack contains a special file".into());
            } else if path
                .extension()
                .is_some_and(|s| s.eq_ignore_ascii_case("wav"))
            {
                let mut header = [0u8; 12];
                File::open(&path)
                    .and_then(|mut f| f.read_exact(&mut header))
                    .map_err(|e| format!("Unreadable sound {}: {e}", path.display()))?;
                let flac = &header[..4] == b"fLaC" && meta.len() >= 42;
                let wav = &header[..4] == b"RIFF" && &header[8..] == b"WAVE" && meta.len() >= 44;
                if !flac && !wav {
                    return Err(format!("Unsupported audio data: {}", path.display()));
                }
                samples += 1;
                bytes = bytes
                    .checked_add(meta.len())
                    .ok_or("Sound pack size overflow")?;
                if bytes > 16 * 1024 * 1024 * 1024 {
                    return Err("Sound pack exceeds 16 GiB".into());
                }
            }
        }
    }
    if samples == 0 {
        return Err("Sound pack contains no imported sounds".into());
    }
    Ok(format!(
        "{samples} sounds · {:.1} MB",
        bytes as f64 / 1_000_000.0
    ))
}

impl AssetReadiness {
    pub fn d2r_ready(&self) -> bool {
        self.d2r_mapping.ready
            && self.d2r_environment.ready
            && self.d2r_ui.ready
            && self.d2r_portal.ready
    }
}

#[derive(Clone, Debug, Default)]
pub struct AssetCandidates {
    pub data_dirs: Vec<PathBuf>,
    pub hd_dirs: Vec<PathBuf>,
    pub d2r_dirs: Vec<PathBuf>,
    pub d2r_installs: Vec<PathBuf>,
    pub importers: Vec<PathBuf>,
    pub pythons: Vec<PathBuf>,
}

/// A portable contained relative path, including Windows rules on Unix hosts.
pub(crate) fn relative_path(name: &str) -> Result<PathBuf, String> {
    if name.is_empty() || name.starts_with(['/', '\\']) || name.contains('\0') {
        return Err(format!("Unsafe asset path: {name:?}"));
    }
    let mut path = PathBuf::new();
    for part in name.split(['/', '\\']) {
        if part.is_empty() {
            continue;
        }
        if part == "." || part == ".." || part.contains(':') || part.ends_with(['.', ' ']) {
            return Err(format!("Unsafe asset path: {name:?}"));
        }
        let stem = part.split('.').next().unwrap_or("").to_ascii_uppercase();
        if matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL")
            || (stem.len() == 4
                && (stem.starts_with("COM") || stem.starts_with("LPT"))
                && stem.as_bytes()[3].is_ascii_digit())
        {
            return Err(format!("Reserved Windows asset path: {name:?}"));
        }
        path.push(part);
    }
    if path.as_os_str().is_empty()
        || path
            .components()
            .any(|c| !matches!(c, Component::Normal(_)))
    {
        return Err(format!("Unsafe asset path: {name:?}"));
    }
    Ok(path)
}

fn regular_file(path: &Path) -> Result<(), String> {
    let meta =
        fs::symlink_metadata(path).map_err(|e| format!("Missing {}: {e}", path.display()))?;
    if !meta.is_file() || meta.file_type().is_symlink() || meta.len() == 0 {
        return Err(format!("Not a nonempty regular file: {}", path.display()));
    }
    Ok(())
}

fn contained_file(root: &Path, name: &str) -> Result<PathBuf, String> {
    contained_file_with_policy(root, name, false)
}

/// Explicitly selected HD references may follow owned local pack links read-only.
/// Imported trees and all D2R manifests always use the strict policy.
fn contained_file_with_policy(
    root: &Path,
    name: &str,
    linked_hd_reference: bool,
) -> Result<PathBuf, String> {
    let relative = relative_path(name)?;
    let path = root.join(&relative);
    if linked_hd_reference {
        let target = path
            .canonicalize()
            .map_err(|e| format!("Cannot resolve HD reference {}: {e}", path.display()))?;
        regular_file(&target)?;
    } else {
        let mut component_path = root.to_path_buf();
        for part in relative.components() {
            component_path.push(part);
            if fs::symlink_metadata(&component_path).is_ok_and(|m| m.file_type().is_symlink()) {
                return Err(format!("Asset references a symlink: {}. Imports require regular files; linked HD packs may be selected read-only with Use folder", component_path.display()));
            }
        }
        regular_file(&path)?;
    }
    if path
        .extension()
        .is_some_and(|v| v.to_string_lossy().eq_ignore_ascii_case("glb"))
    {
        let mut header = [0; 12];
        File::open(&path)
            .and_then(|mut f| f.read_exact(&mut header))
            .map_err(|e| format!("Invalid GLB {}: {e}", path.display()))?;
        if &header[..4] != b"glTF"
            || u32::from_le_bytes(header[4..8].try_into().unwrap()) != 2
            || u32::from_le_bytes(header[8..12].try_into().unwrap()) as u64
                != fs::metadata(&path).map_err(|e| e.to_string())?.len()
        {
            return Err(format!("Invalid GLB header: {}", path.display()));
        }
    }
    Ok(path)
}

fn json(path: &Path) -> Result<Value, String> {
    regular_file(path)?;
    let size = fs::metadata(path).map_err(|e| e.to_string())?.len();
    if size > 16 * 1024 * 1024 {
        return Err(format!("Manifest is too large: {}", path.display()));
    }
    serde_json::from_reader(File::open(path).map_err(|e| e.to_string())?)
        .map_err(|e| format!("Invalid {}: {e}", path.display()))
}

fn reference_fields(value: &Value, fields: &[&str], out: &mut BTreeSet<String>) {
    for field in fields {
        if let Some(path) = value[*field].as_str().filter(|path| !path.is_empty()) {
            out.insert(path.to_owned());
        }
    }
}

/// File fields are schema-specific. Native UI layouts also contain `height`
/// tokens and terrain channel metadata uses `normal`/`albedo` for color spaces.
fn references(value: &Value, kind: &str, out: &mut BTreeSet<String>) {
    match kind {
        "mapping" => {
            if let Some(entries) = value["entries"].as_array() {
                for entry in entries {
                    reference_fields(entry, &["glb"], out);
                    for option in entry["options"].as_array().into_iter().flatten() {
                        reference_fields(option, &["glb"], out);
                    }
                }
            }
        }
        "environment" => {
            if let Some(materials) = value["materials"].as_object() {
                for material in materials.values() {
                    reference_fields(material, &["albedo", "normal", "orm", "height"], out);
                }
            }
            if let Some(models) = value["models"].as_object() {
                for model in models.values() {
                    reference_fields(model, &["glb"], out);
                    for lod in model["contract"]["lods"].as_array().into_iter().flatten() {
                        reference_fields(lod, &["glb"], out);
                    }
                }
            }
        }
        "ui" => {
            for collection in ["images", "fonts"] {
                if let Some(entries) = value[collection].as_object() {
                    for entry in entries.values() {
                        reference_fields(entry, &["file"], out);
                    }
                }
            }
        }
        "portal" => {
            if let Some(textures) = value["textures"].as_object() {
                for texture in textures.values() {
                    reference_fields(texture, &["png"], out);
                }
            }
            if let Some(meshes) = value["meshes"].as_object() {
                for mesh in meshes.values() {
                    reference_fields(mesh, &["glb"], out);
                }
            }
        }
        "hd" => {
            for collection in ["models", "characters"] {
                for entry in value[collection].as_array().into_iter().flatten() {
                    reference_fields(entry, &["file"], out);
                    if let Some(variants) = entry["variants"].as_object() {
                        out.extend(variants.keys().cloned());
                    }
                }
            }
        }
        _ => unreachable!(),
    }
}

fn check_references(root: &Path, value: &Value, kind: &str) -> Result<usize, String> {
    let mut names = BTreeSet::new();
    references(value, kind, &mut names);
    for name in &names {
        contained_file(root, name)?;
    }
    Ok(names.len())
}

fn hd_marker(path: &Path) -> bool {
    [
        "sprites.txt",
        "ui.txt",
        "tiles-town.txt",
        "3d/characters.json",
        "3d/free/manifest.json",
        "3d/hd/manifest.json",
    ]
    .iter()
    .any(|name| path.join(name).is_file())
}

/// Accept the actual `hd` directory, a pack root containing it, or one wrapper.
/// Recognizes a 3D-only add-on for overlay installation; readiness remains separate.
pub fn normalize_hd(path: &Path) -> Result<PathBuf, String> {
    normalize_hd_with_policy(path, false)
}

fn normalize_hd_with_policy(path: &Path, linked_hd_reference: bool) -> Result<PathBuf, String> {
    let meta = if linked_hd_reference {
        fs::metadata(path)
    } else {
        fs::symlink_metadata(path)
    }
    .map_err(|e| format!("Cannot read HD folder: {e}"))?;
    if !meta.is_dir() || (!linked_hd_reference && meta.file_type().is_symlink()) {
        return Err("HD imports require a regular folder; linked packs may be selected read-only with Use folder".into());
    }
    let mut candidates = Vec::new();
    for candidate in [path.to_path_buf(), path.join("hd")] {
        if hd_marker(&candidate) {
            candidates.push(candidate);
        }
    }
    if candidates.is_empty() {
        let mut children = fs::read_dir(path)
            .map_err(|e| e.to_string())?
            .take(65)
            .collect::<Result<Vec<_>, _>>()
            .map_err(|e| e.to_string())?;
        if children.len() > 64 {
            return Err("No HD root found; select the folder containing hd directly".into());
        }
        children.sort_by_key(|e| e.file_name());
        for child in children {
            if if linked_hd_reference {
                child.path().is_dir()
            } else {
                child.file_type().map_err(|e| e.to_string())?.is_dir()
            } {
                for candidate in [child.path(), child.path().join("hd")] {
                    if hd_marker(&candidate) {
                        candidates.push(candidate);
                    }
                }
            }
        }
    }
    candidates.sort();
    candidates.dedup();
    if candidates.len() != 1 {
        return Err(format!(
            "Expected one recognizable HD root, found {}",
            candidates.len()
        ));
    }
    let candidate = candidates.remove(0);
    if !linked_hd_reference
        && fs::symlink_metadata(&candidate)
            .map_err(|e| e.to_string())?
            .file_type()
            .is_symlink()
    {
        return Err(
            "HD import root cannot be a symlink; select it read-only with Use folder".into(),
        );
    }
    Ok(candidate)
}

pub(crate) fn hd_status(path: &Path) -> Result<String, String> {
    hd_status_with_policy(path, false)
}

fn hd_status_with_policy(path: &Path, linked_hd_reference: bool) -> Result<String, String> {
    let root = normalize_hd_with_policy(path, linked_hd_reference)?;
    let mut indexes = 0;
    let mut refs = 0;
    let mut has_3d = false;
    for entry in fs::read_dir(&root).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let name = entry.file_name().to_string_lossy().into_owned();
        if !matches!(
            name.as_str(),
            "sprites.txt" | "items.txt" | "objects.txt" | "ui.txt" | "town-floor.txt"
        ) && !(name.starts_with("tiles-") && name.ends_with(".txt"))
        {
            continue;
        }
        let index = contained_file_with_policy(&root, &name, linked_hd_reference)?;
        if fs::metadata(&index).map_err(|e| e.to_string())?.len() > 16 * 1024 * 1024 {
            return Err("HD index exceeds supported size".into());
        }
        let mut text = String::new();
        File::open(index)
            .map_err(|e| e.to_string())?
            .take(16 * 1024 * 1024 + 1)
            .read_to_string(&mut text)
            .map_err(|e| e.to_string())?;
        if text.len() > 16 * 1024 * 1024 {
            return Err("HD index exceeds supported size".into());
        }
        let mut count = 0;
        for line in text.lines().filter(|l| !l.trim_start().starts_with('#')) {
            let fields: Vec<_> = line.split_whitespace().collect();
            if fields.len() >= 3 && matches!(fields[0], "clip" | "atlas" | "sheet" | "page") {
                contained_file_with_policy(&root, fields[2], linked_hd_reference)?;
                count += 1;
            }
        }
        if count > 0 {
            indexes += 1;
            refs += count;
        }
    }
    for name in [
        "3d/free/manifest.json",
        "3d/hd/manifest.json",
        "3d/characters.json",
    ] {
        let path = root.join(name);
        if path.exists() {
            contained_file_with_policy(&root, name, linked_hd_reference)?;
            let readable = if linked_hd_reference {
                path.canonicalize().map_err(|e| e.to_string())?
            } else {
                path.clone()
            };
            let value = json(&readable)?;
            let manifest_root = path.parent().unwrap();
            let mut names = BTreeSet::new();
            references(&value, "hd", &mut names);
            for name in names {
                if let Some(relative) = name.strip_prefix("hd/") {
                    contained_file_with_policy(&root, relative, linked_hd_reference)?;
                } else {
                    contained_file_with_policy(manifest_root, &name, linked_hd_reference)?;
                }
            }
            has_3d = true;
        }
    }
    if indexes == 0 {
        return Err(if has_3d {
            "3D add-on found; install it over a base HD pack"
        } else {
            "HD indexes have no usable referenced images"
        }
        .into());
    }
    Ok(format!(
        "{indexes} HD indexes, {refs} referenced images{}",
        if has_3d { "; 3D add-on present" } else { "" }
    ))
}

pub fn normalize_d2r(path: &Path) -> PathBuf {
    if path.join("glb").is_dir() {
        path.join("glb")
    } else {
        path.to_path_buf()
    }
}

pub fn is_d2r_install(path: &Path) -> bool {
    path.join(".build.info").is_file() && path.join("Data").is_dir()
}

fn game_status(path: Option<&Path>) -> AssetStatus {
    let Some(path) = path.filter(|path| !path.as_os_str().is_empty()) else {
        return AssetStatus::result(
            None,
            Err("Choose the game executable in Launch options".into()),
        );
    };
    let mut canonical = None;
    let result = (|| {
        let mut executable = path.to_path_buf();
        if cfg!(target_os = "macos")
            && path
                .extension()
                .is_some_and(|extension| extension.eq_ignore_ascii_case("app"))
        {
            for name in ["damnationx_host", "devilutionx", "DamnationX"] {
                let candidate = path.join("Contents/MacOS").join(name);
                if candidate.is_file() {
                    executable = candidate;
                    break;
                }
            }
        }
        let executable = executable
            .canonicalize()
            .map_err(|e| format!("Cannot resolve game executable {}: {e}", path.display()))?;
        regular_file(&executable)?;
        canonical = Some(executable);
        Ok("Game executable found".into())
    })();
    AssetStatus::result(canonical.or_else(|| Some(path.to_path_buf())), result)
}

fn base_status(path: Option<&Path>) -> AssetStatus {
    let Some(path) = path else {
        return AssetStatus::result(
            None,
            Err("Choose the folder with DIABDAT.MPQ or SPAWN.MPQ".into()),
        );
    };
    let root = if path.is_file() {
        path.parent().unwrap_or(path)
    } else {
        path
    };
    let result = (|| {
        for entry in fs::read_dir(root).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            if matches!(
                entry
                    .file_name()
                    .to_string_lossy()
                    .to_ascii_lowercase()
                    .as_str(),
                "diabdat.mpq" | "spawn.mpq"
            ) {
                regular_file(&entry.path())?;
                let mut header = [0; 4];
                File::open(entry.path())
                    .and_then(|mut f| f.read_exact(&mut header))
                    .map_err(|e| e.to_string())?;
                if header != *b"MPQ\x1a" && header != *b"MPQ\x1b" {
                    return Err("Base archive has no MPQ signature".into());
                }
                return Ok(format!(
                    "Owned base archive: {}",
                    entry.file_name().to_string_lossy()
                ));
            }
        }
        Err("DIABDAT.MPQ or SPAWN.MPQ not found".into())
    })();
    AssetStatus::result(Some(root.to_path_buf()), result)
}

pub fn inspect(paths: &AssetPaths) -> AssetReadiness {
    let hd_root = paths
        .hd_dir
        .as_deref()
        .and_then(|p| normalize_hd_with_policy(p, true).ok());
    let hd = AssetStatus::result(
        hd_root.clone().or_else(|| paths.hd_dir.clone()),
        paths
            .hd_dir
            .as_deref()
            .ok_or("Choose an HD pack".into())
            .and_then(|path| hd_status_with_policy(path, true)),
    );
    let root = paths.d2r_dir.as_deref().map(normalize_d2r);
    let status = |relative: &str, kind: &str| -> AssetStatus {
        let result = (|| {
            let root = root
                .as_deref()
                .ok_or("Select or import the converted D2R cache")?;
            let file = root.join(relative);
            let value = json(&file)?;
            match kind {
                "mapping" => {
                    let entries = value["entries"]
                        .as_array()
                        .filter(|v| !v.is_empty())
                        .ok_or("D2R mapping contains no entries")?;
                    for entry in entries {
                        if entry["model"].as_str().is_some_and(|m| !m.is_empty())
                            && entry["glb"].as_str().is_none()
                        {
                            return Err(format!("Unconverted mapping model: {}", entry["model"]));
                        }
                        if let Some(options) = entry["options"].as_array() {
                            if options.iter().any(|o| {
                                o["model"].as_str().is_some_and(|m| !m.is_empty())
                                    && o["glb"].as_str().is_none()
                            }) {
                                return Err("A hero option has not been converted".into());
                            }
                        }
                        for model in std::iter::once(entry)
                            .chain(entry["options"].as_array().into_iter().flatten())
                        {
                            if model["static"].as_bool() != Some(true) {
                                if let Some(clips) = model["clips"].as_object() {
                                    for (name, clip) in clips {
                                        if !name.starts_with("town_")
                                            && name != "talk"
                                            && clip.is_null()
                                        {
                                            return Err(format!(
                                                "Model {} has an unresolved {name} animation",
                                                model["model"]
                                            ));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                "environment" => {
                    for level in ["town", "cathedral", "catacombs", "caves", "hell"] {
                        if value["levels"].get(level).is_none() {
                            return Err(format!("Environment is missing {level}"));
                        }
                    }
                    if value["models"].as_object().is_none_or(|v| v.is_empty()) {
                        return Err("Environment has no models".into());
                    }
                }
                "ui" => {
                    for key in [
                        "panel_background",
                        "button_sprint",
                        "options_background",
                        "options_pause_button",
                        "options_tabs",
                        "options_checkbox",
                        "options_slider_track",
                        "options_slider_thumb",
                    ] {
                        if value["images"].get(key).is_none() {
                            return Err(format!("D2R UI is missing {key}"));
                        }
                    }
                }
                "portal" => {
                    for key in [
                        "t_fx_noise_cells_conviction",
                        "t_fx_noise_cells_conviction_glow_02_hilights",
                        "t_fx_noise_goopy_03_meshdissolve",
                        "t_fx_portal_gradient_03",
                    ] {
                        if value["textures"].get(key).is_none() {
                            return Err(format!("Portal is missing {key}"));
                        }
                    }
                }
                _ => unreachable!(),
            }
            let count = check_references(
                if kind == "ui" {
                    file.parent().unwrap()
                } else {
                    root
                },
                &value,
                kind,
            )?;
            if count == 0 {
                return Err("Manifest contains no referenced assets".into());
            }
            Ok(format!(
                "{kind}: manifest and {count} referenced files found"
            ))
        })();
        AssetStatus::result(root.clone(), result)
    };
    let sound_mods = paths
        .sound_mods
        .clone()
        .or_else(|| CachePaths::for_user().ok().map(|p| p.sound_mods));
    let sound_status = |name: &str| {
        let path = sound_mods.as_ref().map(|p| p.join(name));
        let result = path
            .as_deref()
            .ok_or("Import sounds from your owned D2R installation".into())
            .and_then(sound_pack_status);
        AssetStatus::result(path, result)
    };
    AssetReadiness {
        game: game_status(paths.game.as_deref()),
        base: base_status(paths.data_dir.as_deref()),
        hd,
        d2r_mapping: status("mapping.resolved.json", "mapping"),
        d2r_environment: status("env/env.resolved.json", "environment"),
        d2r_ui: status("ui/ui.resolved.json", "ui"),
        d2r_portal: status("vfx/town_portal/portal.resolved.json", "portal"),
        d2r_sounds_classic: sound_status(D2R_SOUND_PACKS[0]),
        d2r_sounds_remastered: sound_status(D2R_SOUND_PACKS[1]),
    }
}

pub fn detect() -> AssetCandidates {
    let mut found = AssetCandidates::default();
    let mut roots = Vec::new();
    if let Ok(paths) = CachePaths::for_user() {
        found
            .hd_dirs
            .extend(normalize_hd_with_policy(&paths.hd_dir, true));
        let glb = paths.d2r_cache.join("glb");
        if glb.join("mapping.resolved.json").is_file() {
            found.d2r_dirs.push(glb);
        }
        if let Ok(saved) = fs::read_to_string(paths.d2r_cache.join("game_dir.txt")) {
            roots.push(PathBuf::from(saved.trim()));
        }
    }
    for key in [
        "D2R_GAME_DIR",
        "D2R_IMPORT_CACHE",
        "DAMNATIONX_HD_DIR",
        "DAMNATIONX_DATA_DIR",
    ] {
        if let Some(path) = std::env::var_os(key).map(PathBuf::from) {
            roots.push(path);
        }
    }
    if let Ok(cwd) = std::env::current_dir() {
        roots.push(cwd);
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            roots.push(parent.to_path_buf());
            roots.push(parent.join("resources/d2r-import"));
            roots.push(parent.join("../Resources/d2r-import"));
        }
    }
    if let Some(home) = directories::BaseDirs::new().map(|d| d.home_dir().to_path_buf()) {
        roots.push(home.join("Library/Application Support/diasurgical/devilution"));
        roots.push(home.join("Library/Application Support/damnationx/hd-combined/hd"));
        roots.push(home.join("Library/Application Support/damnationx/d2r-models/glb"));
        roots.push(home.join(".local/share/diasurgical/devilution"));
        roots.push(home.join(".wine/drive_c/Program Files (x86)/Diablo II Resurrected"));
        roots.push(home.join(".wine/drive_c/Program Files/Diablo II Resurrected"));
        for bottle in [
            home.join("Library/Application Support/CrossOver/Bottles"),
            home.join(".wine"),
        ] {
            if let Ok(children) = fs::read_dir(&bottle) {
                for child in children.take(64).flatten() {
                    roots.push(
                        child
                            .path()
                            .join("drive_c/Program Files (x86)/Diablo II Resurrected"),
                    );
                    roots.push(
                        child
                            .path()
                            .join("drive_c/Program Files/Diablo II Resurrected"),
                    );
                }
            }
        }
    }
    if let Some(dirs) = directories::BaseDirs::new() {
        roots.push(dirs.config_dir().join("diasurgical/devilution"));
    }
    for key in ["PROGRAMFILES", "PROGRAMFILES(X86)"] {
        if let Some(path) = std::env::var_os(key).map(PathBuf::from) {
            roots.push(path.join("Diablo II Resurrected"));
        }
    }
    roots.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../d2r-import"));
    for path in roots {
        if base_status(Some(&path)).ready {
            found.data_dirs.push(path.clone());
        }
        if let Ok(hd) = normalize_hd_with_policy(&path, true) {
            found.hd_dirs.push(hd);
        }
        let glb = normalize_d2r(&path);
        if glb.join("mapping.resolved.json").is_file() {
            found.d2r_dirs.push(glb);
        }
        if is_d2r_install(&path) {
            found.d2r_installs.push(path.clone());
        }
        if path.join("d2r_import/__main__.py").is_file() && path.join("requirements.txt").is_file()
        {
            found.importers.push(path);
        }
    }
    if let Some(paths) = std::env::var_os("PATH") {
        for directory in std::env::split_paths(&paths) {
            // Windows Store execution aliases are not installed interpreters.
            if directory.components().any(|c| {
                c.as_os_str()
                    .to_string_lossy()
                    .eq_ignore_ascii_case("WindowsApps")
            }) {
                continue;
            }
            for name in if cfg!(windows) {
                vec!["python.exe", "python3.exe", "py.exe"]
            } else {
                vec!["python3", "python"]
            } {
                let path = directory.join(name);
                if path.is_file() {
                    found.pythons.push(path);
                }
            }
        }
    }
    for list in [
        &mut found.data_dirs,
        &mut found.hd_dirs,
        &mut found.d2r_dirs,
        &mut found.d2r_installs,
        &mut found.importers,
        &mut found.pythons,
    ] {
        list.sort();
        list.dedup();
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sound_pack_needs_completed_manifest_and_audio_bytes() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        fs::create_dir(root.join("sfx")).unwrap();
        let sample = root.join("sfx/test.wav");
        let mut wave = vec![0u8; 44];
        wave[..4].copy_from_slice(b"RIFF");
        wave[8..12].copy_from_slice(b"WAVE");
        fs::write(&sample, &wave).unwrap();
        assert!(
            sound_pack_status(root).is_err(),
            "unfinished imports are unavailable"
        );
        fs::write(
            root.join("manifest.ini"),
            "[mod]\nname=source-free sound fixture\n",
        )
        .unwrap();
        assert!(sound_pack_status(root).unwrap().starts_with("1 sounds"));
        fs::write(&sample, b"not an audio container even with a WAV suffix").unwrap();
        assert!(sound_pack_status(root)
            .unwrap_err()
            .contains("Unsupported audio data"));
        fs::remove_file(&sample).unwrap();
        assert!(sound_pack_status(root)
            .unwrap_err()
            .contains("no imported sounds"));
        #[cfg(unix)]
        {
            let outside = root.join("outside.wav");
            fs::write(&outside, &wave).unwrap();
            std::os::unix::fs::symlink(&outside, &sample).unwrap();
            assert!(sound_pack_status(root).unwrap_err().contains("link"));
        }
    }
    #[test]
    fn rejects_cross_platform_traversal_and_device_paths() {
        for name in [
            "../escape",
            "a/../../escape",
            "/root",
            "C:\\root",
            "a\\..\\escape",
            "file:stream",
            "NUL.txt",
            "a/CON",
            "name. ",
        ] {
            assert!(relative_path(name).is_err(), "{name}");
        }
        assert_eq!(
            relative_path("hd\\sprites\\hero.png").unwrap(),
            PathBuf::from("hd/sprites/hero.png")
        );
    }
    #[test]
    fn hd_readiness_requires_referenced_images_and_normalizes_wrapper() {
        let temp = tempfile::tempdir().unwrap();
        let hd = temp.path().join("bundle/hd");
        fs::create_dir_all(hd.join("sprites")).unwrap();
        fs::write(
            hd.join("sprites.txt"),
            "clip hero/idle sprites/hero.png 1 1 1 1 0 0 1\n",
        )
        .unwrap();
        assert_eq!(normalize_hd(temp.path()).unwrap(), hd);
        assert!(hd_status(temp.path()).is_err());
        fs::write(hd.join("sprites/hero.png"), b"image").unwrap();
        assert!(hd_status(temp.path()).is_ok());
    }
    #[test]
    fn hd_tile_images_are_not_parsed_as_text_indexes() {
        let temp = tempfile::tempdir().unwrap();
        fs::write(
            temp.path().join("tiles-town.txt"),
            "atlas 2 tiles-town-2.webp 4 4\n",
        )
        .unwrap();
        fs::write(
            temp.path().join("tiles-town-2.webp"),
            [0xff, 0x98, 0x00, 0x80],
        )
        .unwrap();
        assert!(hd_status(temp.path()).is_ok());
    }
    #[cfg(unix)]
    #[test]
    fn hd_reference_links_are_readonly_and_imports_stay_strict() {
        let temp = tempfile::tempdir().unwrap();
        let hd = temp.path().join("hd");
        fs::create_dir(&hd).unwrap();
        let image = temp.path().join("owned.webp");
        fs::write(&image, b"image").unwrap();
        fs::write(hd.join("tiles-town.txt"), "atlas 0 tile.webp 4 4\n").unwrap();
        std::os::unix::fs::symlink(&image, hd.join("tile.webp")).unwrap();
        let paths = AssetPaths {
            hd_dir: Some(hd.clone()),
            ..Default::default()
        };
        assert!(inspect(&paths).hd.ready);
        assert!(hd_status(&hd)
            .unwrap_err()
            .contains("Imports require regular files"));

        std::os::unix::fs::symlink(temp.path(), hd.join("linked-parent")).unwrap();
        fs::write(
            hd.join("tiles-town.txt"),
            "atlas 0 linked-parent/owned.webp 4 4\n",
        )
        .unwrap();
        let status = inspect(&paths);
        assert!(status.hd.ready);
        assert!(hd_status(&hd)
            .unwrap_err()
            .contains("Imports require regular files"));

        let index = temp.path().join("owned-index.txt");
        fs::write(&index, "atlas 0 linked-parent/owned.webp 4 4\n").unwrap();
        fs::remove_file(hd.join("tiles-town.txt")).unwrap();
        std::os::unix::fs::symlink(index, hd.join("tiles-town.txt")).unwrap();
        assert!(inspect(&paths).hd.ready);
        assert!(hd_status(&hd)
            .unwrap_err()
            .contains("Imports require regular files"));

        let linked_root = temp.path().join("linked-hd");
        std::os::unix::fs::symlink(&hd, &linked_root).unwrap();
        let paths = AssetPaths {
            hd_dir: Some(linked_root.clone()),
            ..Default::default()
        };
        assert!(inspect(&paths).hd.ready);
        assert!(normalize_hd_with_policy(&linked_root, true).is_ok());
        assert!(normalize_hd(&linked_root).is_err());
    }
    #[test]
    fn d2r_schema_contexts_ignore_channel_metadata_and_layout_tokens() {
        use serde_json::json;
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        for directory in ["env", "ui", "vfx/town_portal"] {
            fs::create_dir_all(root.join(directory)).unwrap();
        }
        let model_json = br#"{"asset":{"version":"2.0"}}  "#;
        let mut glb = b"glTF".to_vec();
        glb.extend(2u32.to_le_bytes());
        glb.extend((20u32 + model_json.len() as u32).to_le_bytes());
        glb.extend((model_json.len() as u32).to_le_bytes());
        glb.extend(b"JSON");
        glb.extend(model_json);
        fs::write(root.join("hero.glb"), glb).unwrap();
        fs::write(root.join("env/stone.png"), b"image").unwrap();
        fs::write(root.join("ui/interface.png"), b"image").unwrap();
        fs::write(root.join("ui/font.otf"), b"font").unwrap();
        fs::write(root.join("vfx/town_portal/field.png"), b"image").unwrap();
        let mapping =
            json!({"entries":[{"model":"hero", "glb":"hero.glb", "clips":{"stand":"idle"}}]});
        let environment = json!({
            "levels":{"town":{},"cathedral":{},"catacombs":{},"caves":{},"hell":{}},
            "materials":{"stone":{"albedo":"env/stone.png", "normal":"env/stone.png", "orm":"env/stone.png",
                "channels":{"albedo":"srgb", "normal":"linear", "orm":"linear: R=AO G=roughness B=metallic"},
                "texture_sha256":{"albedo":"hash-is-not-a-file"}}},
            "models":{"hero":{"glb":"hero.glb", "contract":{"lods":[{"glb":"hero.glb"}]}}}
        });
        let mut images = serde_json::Map::new();
        for key in [
            "panel_background",
            "button_sprint",
            "options_background",
            "options_pause_button",
            "options_tabs",
            "options_checkbox",
            "options_slider_track",
            "options_slider_thumb",
        ] {
            images.insert(key.into(), json!({"file":"interface.png", "height":4}));
        }
        let ui = json!({"images":images, "fonts":{"exocet":{"file":"font.otf"}},
            "options":{"profile_values":{"OptionsPanelRect":{"height":"$OptionsPanelHeight"}}}});
        let mut textures = serde_json::Map::new();
        for key in [
            "t_fx_noise_cells_conviction",
            "t_fx_noise_cells_conviction_glow_02_hilights",
            "t_fx_noise_goopy_03_meshdissolve",
            "t_fx_portal_gradient_03",
        ] {
            textures.insert(
                key.into(),
                json!({"png":"vfx/town_portal/field.png", "height":512}),
            );
        }
        let portal = json!({"textures":textures,"meshes":{"disk":{"glb":"hero.glb"}}});
        for (name, value) in [
            ("mapping.resolved.json", mapping),
            ("env/env.resolved.json", environment),
            ("ui/ui.resolved.json", ui),
            ("vfx/town_portal/portal.resolved.json", portal),
        ] {
            fs::write(root.join(name), serde_json::to_vec(&value).unwrap()).unwrap();
        }
        let paths = AssetPaths {
            d2r_dir: Some(root.to_path_buf()),
            ..Default::default()
        };
        let status = inspect(&paths);
        assert!(status.d2r_ready(), "{status:?}");
        fs::remove_file(root.join("env/stone.png")).unwrap();
        assert!(!inspect(&paths).d2r_environment.ready);
        assert!(inspect(&paths).d2r_ui.ready);
    }
    #[test]
    fn executable_is_canonicalized_on_inspection_and_must_be_nonempty() {
        let temp = tempfile::tempdir().unwrap();
        let executable = temp.path().join("game");
        fs::write(&executable, b"fixture").unwrap();
        let status = inspect(&AssetPaths {
            game: Some(executable.clone()),
            ..Default::default()
        });
        assert!(status.game.ready);
        assert_eq!(status.game.path, Some(executable.canonicalize().unwrap()));
        fs::write(&executable, []).unwrap();
        assert!(
            !inspect(&AssetPaths {
                game: Some(executable),
                ..Default::default()
            })
            .game
            .ready
        );
    }
    #[cfg(target_os = "macos")]
    #[test]
    fn macos_app_bundle_is_resolved_on_inspection() {
        let temp = tempfile::tempdir().unwrap();
        let bundle = temp.path().join("DamnationX.app");
        let executable = bundle.join("Contents/MacOS/damnationx_host");
        fs::create_dir_all(executable.parent().unwrap()).unwrap();
        fs::write(&executable, b"fixture").unwrap();
        let status = inspect(&AssetPaths {
            game: Some(bundle),
            ..Default::default()
        });
        assert!(status.game.ready);
        assert_eq!(status.game.path, Some(executable.canonicalize().unwrap()));
    }
    #[test]
    fn empty_base_or_d2r_manifest_is_not_launch_ready() {
        let temp = tempfile::tempdir().unwrap();
        fs::write(temp.path().join("DIABDAT.MPQ"), b"not an mpq").unwrap();
        fs::write(temp.path().join("mapping.resolved.json"), "{}").unwrap();
        let status = inspect(&AssetPaths {
            data_dir: Some(temp.path().into()),
            d2r_dir: Some(temp.path().into()),
            ..Default::default()
        });
        assert!(!status.base.ready && !status.d2r_ready());
        fs::write(temp.path().join("DIABDAT.MPQ"), b"MPQ\x1aowned fixture").unwrap();
        assert!(
            inspect(&AssetPaths {
                data_dir: Some(temp.path().into()),
                ..Default::default()
            })
            .base
            .ready
        );
    }
    #[test]
    fn oversized_hd_index_is_rejected_before_reading_it() {
        let temp = tempfile::tempdir().unwrap();
        let file = File::create(temp.path().join("sprites.txt")).unwrap();
        file.set_len(16 * 1024 * 1024 + 1).unwrap();
        assert_eq!(
            hd_status(temp.path()).unwrap_err(),
            "HD index exceeds supported size"
        );
    }
}
