//! Import stamps for owned D2R conversions and the matching "clear" operation.
//!
//! A stamp records what produced a published import (the owned install and its
//! build, the importer or game executable that converted it) and what was
//! published (every output file and its size). A repeated import whose stamp
//! still matches is skipped. When anything differs the full pipeline runs; the
//! `d2r_resume` additionally retains private work and phase checkpoints across
//! failures, cancellations and publication, without changing playable assets.
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs::{self, OpenOptions};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use crate::assets::D2R_SOUND_PACKS;

/// Stamp for converted art, kept beside (not inside) the published `glb` tree.
pub const ART_STAMP: &str = "import-stamp.json";
/// Stamp for the two sound packs. It lives in the launcher's D2R cache so the
/// game's mods folder only ever contains the packs themselves.
pub const SOUND_STAMP: &str = "sounds-stamp.json";
/// Name of the launcher-managed art cache. Clearing refuses any other folder.
pub const MANAGED_CACHE_NAME: &str = "d2r-models";
const STAMP_VERSION: u32 = 1;
const HASH_FILE_LIMIT: u64 = 16 * 1024 * 1024;

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct SourceFingerprint {
    /// Canonical owned D2R installation.
    pub game_dir: PathBuf,
    /// `Version` from `.build.info`, for logs and the hint text.
    pub build: String,
    /// Hash of `.build.info`; it changes with every game patch.
    pub build_info: u64,
    /// Importer recipes (art) or the converting executable (sounds).
    pub converter: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ImportStamp {
    pub version: u32,
    pub launcher: String,
    pub source: SourceFingerprint,
    /// Output path relative to the stamped root, with `/` separators -> bytes.
    pub outputs: BTreeMap<String, u64>,
}

impl ImportStamp {
    pub fn new(source: SourceFingerprint, outputs: BTreeMap<String, u64>) -> Self {
        Self {
            version: STAMP_VERSION,
            launcher: env!("CARGO_PKG_VERSION").into(),
            source,
            outputs,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum CacheDecision {
    /// Everything recorded is still present and the source is unchanged.
    Skip,
    /// Import again; the reason is shown in the job log.
    Import(String),
}

/// Pure skip decision. `actual` is the current output inventory, or the error
/// that prevented reading it.
pub fn decide(
    stamp: Option<&ImportStamp>,
    source: &SourceFingerprint,
    actual: Result<&BTreeMap<String, u64>, &str>,
) -> CacheDecision {
    let Some(stamp) = stamp else {
        return CacheDecision::Import("no previous import recorded".into());
    };
    if stamp.version != STAMP_VERSION {
        return CacheDecision::Import("import record is from another launcher format".into());
    }
    if stamp.source.game_dir != source.game_dir {
        return CacheDecision::Import("a different D2R installation was selected".into());
    }
    if stamp.source.build_info != source.build_info {
        return CacheDecision::Import(format!(
            "D2R build changed ({} -> {})",
            stamp.source.build, source.build
        ));
    }
    if stamp.source.converter != source.converter {
        return CacheDecision::Import("the importer changed".into());
    }
    if stamp.outputs.is_empty() {
        return CacheDecision::Import("previous import recorded no files".into());
    }
    let actual = match actual {
        Ok(actual) => actual,
        Err(error) => return CacheDecision::Import(format!("outputs unreadable: {error}")),
    };
    for (path, size) in &stamp.outputs {
        match actual.get(path) {
            None => return CacheDecision::Import(format!("{path} is missing")),
            Some(found) if found != size => {
                return CacheDecision::Import(format!("{path} changed size"))
            }
            _ => {}
        }
    }
    if let Some(extra) = actual.keys().find(|k| !stamp.outputs.contains_key(*k)) {
        return CacheDecision::Import(format!("{extra} was not part of the recorded import"));
    }
    CacheDecision::Skip
}

fn fnv(hash: &mut u64, bytes: &[u8]) {
    for byte in bytes {
        *hash = (*hash ^ *byte as u64).wrapping_mul(1099511628211);
    }
}

const FNV_START: u64 = 14695981039346656037;

fn build_version(text: &str) -> String {
    let mut lines = text.lines();
    let (Some(header), Some(row)) = (lines.next(), lines.next()) else {
        return "?".into();
    };
    header
        .split('|')
        .map(|h| h.split('!').next().unwrap_or(""))
        .zip(row.split('|'))
        .find(|(h, _)| *h == "Version")
        .map(|(_, v)| v.to_owned())
        .unwrap_or_else(|| "?".into())
}

/// Fingerprint an owned install plus a converter hash.
pub fn source_fingerprint(game_dir: &Path, converter: u64) -> Result<SourceFingerprint, String> {
    let game_dir = game_dir.canonicalize().map_err(|e| e.to_string())?;
    let bytes = fs::read(game_dir.join(".build.info"))
        .map_err(|e| format!("Cannot read .build.info: {e}"))?;
    let mut build_info = FNV_START;
    fnv(&mut build_info, &bytes);
    let text = String::from_utf8_lossy(&bytes);
    Ok(SourceFingerprint {
        build: build_version(text.trim_start_matches('\u{feff}')),
        game_dir,
        build_info,
        converter,
    })
}

fn file_identity(hash: &mut u64, path: &Path) -> Result<(), String> {
    let meta = fs::metadata(path).map_err(|e| e.to_string())?;
    fnv(hash, &meta.len().to_le_bytes());
    let modified = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    fnv(hash, &modified.to_le_bytes());
    Ok(())
}

/// Hash of the importer recipes: code, mappings and helper binaries. Docs,
/// tests and bytecode caches are ignored so they never force a re-import.
pub fn importer_fingerprint(importer_dir: &Path) -> Result<u64, String> {
    importer_fingerprint_excluding(importer_dir, &[])
}

pub fn importer_fingerprint_excluding(
    importer_dir: &Path,
    excluded: &[&str],
) -> Result<u64, String> {
    let mut files = Vec::new();
    let mut stack = vec![importer_dir.to_path_buf()];
    while let Some(dir) = stack.pop() {
        for entry in fs::read_dir(&dir).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            let kind = entry.file_type().map_err(|e| e.to_string())?;
            if name.starts_with('.') || name == "__pycache__" || name == "tests" {
                continue;
            }
            if kind.is_dir() {
                stack.push(path);
            } else if kind.is_file() && !name.ends_with(".md") && !name.ends_with(".pyc") {
                files.push(path);
            }
        }
    }
    files.sort();
    let mut hash = FNV_START;
    for file in files {
        let relative = file.strip_prefix(importer_dir).unwrap_or(&file);
        let portable = relative.to_string_lossy().replace('\\', "/");
        if excluded.contains(&portable.as_str()) {
            continue;
        }
        fnv(&mut hash, relative.to_string_lossy().as_bytes());
        let len = fs::metadata(&file).map_err(|e| e.to_string())?.len();
        if len <= HASH_FILE_LIMIT {
            fnv(&mut hash, &fs::read(&file).map_err(|e| e.to_string())?);
        } else {
            file_identity(&mut hash, &file)?;
        }
    }
    Ok(hash)
}

/// Streaming content hash for resumable work; sizes alone miss truncated/replaced
/// outputs of the same size. No model or image has to be held in memory.
pub fn output_fingerprint(path: &Path) -> Result<u64, String> {
    let mut file = fs::File::open(path).map_err(|e| e.to_string())?;
    let mut hash = FNV_START;
    let mut buffer = [0; 64 * 1024];
    loop {
        let count = file.read(&mut buffer).map_err(|e| e.to_string())?;
        if count == 0 {
            return Ok(hash);
        }
        fnv(&mut hash, &buffer[..count]);
    }
}

/// Identity of the executable that converts sounds, and how it is run.
pub fn program_fingerprint(program: &Path, classic_runtime: bool) -> Result<u64, String> {
    let mut hash = FNV_START;
    fnv(&mut hash, program.to_string_lossy().as_bytes());
    fnv(&mut hash, &[u8::from(classic_runtime)]);
    file_identity(&mut hash, program)?;
    Ok(hash)
}

/// Every regular file under `root/<subdir>` for each subdir (or the whole root
/// when `subdirs` is empty), keyed relative to `root`. Links are refused.
pub fn scan_outputs(root: &Path, subdirs: &[&str]) -> Result<BTreeMap<String, u64>, String> {
    let mut outputs = BTreeMap::new();
    let mut stack: Vec<PathBuf> = if subdirs.is_empty() {
        vec![root.to_path_buf()]
    } else {
        subdirs.iter().map(|s| root.join(s)).collect()
    };
    while let Some(dir) = stack.pop() {
        let meta = fs::symlink_metadata(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        if !meta.is_dir() {
            return Err(format!("{} is not a folder", dir.display()));
        }
        for entry in fs::read_dir(&dir).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let path = entry.path();
            let meta = fs::symlink_metadata(&path).map_err(|e| e.to_string())?;
            if meta.file_type().is_symlink() {
                return Err(format!("{} is a link", path.display()));
            }
            if meta.is_dir() {
                stack.push(path);
            } else {
                let relative = path
                    .strip_prefix(root)
                    .map_err(|e| e.to_string())?
                    .components()
                    .map(|c| c.as_os_str().to_string_lossy())
                    .collect::<Vec<_>>()
                    .join("/");
                outputs.insert(relative, meta.len());
            }
        }
    }
    Ok(outputs)
}

pub fn load_stamp(path: &Path) -> Option<ImportStamp> {
    serde_json::from_slice(&fs::read(path).ok()?).ok()
}

pub fn write_stamp(path: &Path, stamp: &ImportStamp) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let text = serde_json::to_vec_pretty(stamp).map_err(|e| e.to_string())?;
    let temporary = path.with_extension("json.writing");
    fs::write(&temporary, text).map_err(|e| e.to_string())?;
    fs::rename(&temporary, path).map_err(|e| e.to_string())
}

/// What `clear_import` may remove: the managed art cache (converted models,
/// environment, interface, portal, helper and both stamps) and the two sound
/// packs the launcher writes. Nothing else.
pub fn clear_targets(cache_dir: &Path, sound_mods: &Path) -> Vec<PathBuf> {
    let mut targets = vec![cache_dir.to_path_buf()];
    targets.extend(D2R_SOUND_PACKS.iter().map(|name| sound_mods.join(name)));
    targets
}

fn lock(directory: &Path) -> Result<Option<fs::File>, String> {
    if !directory.is_dir() {
        return Ok(None);
    }
    let file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(directory.join(".damnationx-install.lock"))
        .map_err(|e| e.to_string())?;
    file.try_lock()
        .map_err(|e| format!("An asset import is still running: {e}"))?;
    Ok(Some(file))
}

/// Delete only the launcher-created D2R import outputs. `protected` lists
/// folders that must survive (the owned D2R install, game data, saves); a
/// target that is, contains or lies inside one of them is refused. A link at a
/// target is removed as a link; its destination is never followed.
pub fn clear_import(
    cache_dir: &Path,
    sound_mods: &Path,
    protected: &[PathBuf],
) -> Result<Vec<PathBuf>, String> {
    if !cache_dir.is_absolute() || !sound_mods.is_absolute() {
        return Err("Refusing to clear a relative path".into());
    }
    if cache_dir.file_name().and_then(|n| n.to_str()) != Some(MANAGED_CACHE_NAME) {
        return Err(format!(
            "Refusing to clear {}: not the launcher's {MANAGED_CACHE_NAME} folder",
            cache_dir.display()
        ));
    }
    let canonical = |p: &Path| p.canonicalize().unwrap_or_else(|_| p.to_path_buf());
    let targets = clear_targets(cache_dir, sound_mods);
    for target in &targets {
        let target = canonical(target);
        for keep in protected.iter().map(|p| canonical(p)) {
            if keep.starts_with(&target) || target.starts_with(&keep) {
                return Err(format!(
                    "Refusing to clear {}: it overlaps {}",
                    target.display(),
                    keep.display()
                ));
            }
        }
    }
    // Same locks as the import stages: never delete under a running import.
    let _art = lock(cache_dir)?;
    let _sounds = match sound_mods.parent() {
        Some(parent) => lock(parent)?,
        None => None,
    };
    let mut removed = Vec::new();
    for target in targets {
        let Ok(meta) = fs::symlink_metadata(&target) else {
            continue;
        };
        if meta.is_dir() {
            fs::remove_dir_all(&target)
        } else {
            fs::remove_file(&target)
        }
        .map_err(|e| format!("Could not remove {}: {e}", target.display()))?;
        removed.push(target);
    }
    Ok(removed)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn source(build_info: u64) -> SourceFingerprint {
        SourceFingerprint {
            game_dir: "/owned/d2r".into(),
            build: "1.0".into(),
            build_info,
            converter: 7,
        }
    }

    fn outputs(entries: &[(&str, u64)]) -> BTreeMap<String, u64> {
        entries.iter().map(|(k, v)| (k.to_string(), *v)).collect()
    }

    #[test]
    fn skip_only_when_source_converter_and_outputs_match() {
        let files = outputs(&[("mapping.resolved.json", 10), ("env/a.glb", 20)]);
        let stamp = ImportStamp::new(source(1), files.clone());
        assert_eq!(
            decide(Some(&stamp), &source(1), Ok(&files)),
            CacheDecision::Skip
        );
        assert!(matches!(
            decide(None, &source(1), Ok(&files)),
            CacheDecision::Import(_)
        ));
        // New game patch.
        assert!(
            matches!(decide(Some(&stamp), &source(2), Ok(&files)), CacheDecision::Import(r) if r.contains("build"))
        );
        // Importer updated.
        let mut other = source(1);
        other.converter = 8;
        assert!(matches!(
            decide(Some(&stamp), &other, Ok(&files)),
            CacheDecision::Import(_)
        ));
        // Another install.
        let mut other = source(1);
        other.game_dir = "/elsewhere".into();
        assert!(matches!(
            decide(Some(&stamp), &other, Ok(&files)),
            CacheDecision::Import(_)
        ));
        // Missing, resized and unexpected outputs.
        let missing = outputs(&[("mapping.resolved.json", 10)]);
        assert!(
            matches!(decide(Some(&stamp), &source(1), Ok(&missing)), CacheDecision::Import(r) if r.contains("missing"))
        );
        let resized = outputs(&[("mapping.resolved.json", 10), ("env/a.glb", 21)]);
        assert!(matches!(
            decide(Some(&stamp), &source(1), Ok(&resized)),
            CacheDecision::Import(_)
        ));
        let extra = outputs(&[("mapping.resolved.json", 10), ("env/a.glb", 20), ("x", 1)]);
        assert!(matches!(
            decide(Some(&stamp), &source(1), Ok(&extra)),
            CacheDecision::Import(_)
        ));
        assert!(matches!(
            decide(Some(&stamp), &source(1), Err("gone")),
            CacheDecision::Import(_)
        ));
        // An empty record never counts as an import.
        let empty = ImportStamp::new(source(1), BTreeMap::new());
        assert!(matches!(
            decide(Some(&empty), &source(1), Ok(&BTreeMap::new())),
            CacheDecision::Import(_)
        ));
    }

    #[test]
    fn stamp_round_trip_and_scan_match_on_disk() {
        let temp = tempfile::tempdir().unwrap();
        let glb = temp.path().join("d2r-models/glb");
        fs::create_dir_all(glb.join("env")).unwrap();
        fs::write(glb.join("mapping.resolved.json"), b"{}").unwrap();
        fs::write(glb.join("env/a.glb"), b"glTF").unwrap();
        let game = temp.path().join("owned");
        fs::create_dir_all(game.join("Data")).unwrap();
        fs::write(
            game.join(".build.info"),
            "Branch!STRING:0|Version!STRING:0\nus|1.6.80273\n",
        )
        .unwrap();
        let fingerprint = source_fingerprint(&game, 3).unwrap();
        assert_eq!(fingerprint.build, "1.6.80273");
        let scanned = scan_outputs(&glb, &[]).unwrap();
        assert_eq!(
            scanned,
            outputs(&[("env/a.glb", 4), ("mapping.resolved.json", 2)])
        );
        let path = temp.path().join("d2r-models").join(ART_STAMP);
        write_stamp(&path, &ImportStamp::new(fingerprint.clone(), scanned)).unwrap();
        let loaded = load_stamp(&path).unwrap();
        let again = source_fingerprint(&game, 3).unwrap();
        let now = scan_outputs(&glb, &[]).unwrap();
        assert_eq!(decide(Some(&loaded), &again, Ok(&now)), CacheDecision::Skip);
        // A patched install invalidates the record.
        fs::write(
            game.join(".build.info"),
            "Branch!STRING:0|Version!STRING:0\nus|1.6.90000\n",
        )
        .unwrap();
        let patched = source_fingerprint(&game, 3).unwrap();
        assert!(matches!(
            decide(Some(&loaded), &patched, Ok(&now)),
            CacheDecision::Import(_)
        ));
        // Sound scans cover only the two packs, not other mods.
        let mods = temp.path().join("mods");
        for name in D2R_SOUND_PACKS {
            fs::create_dir_all(mods.join(name)).unwrap();
            fs::write(mods.join(name).join("a.wav"), b"RIFF").unwrap();
        }
        fs::create_dir_all(mods.join("other")).unwrap();
        fs::write(mods.join("other/b.wav"), b"RIFF").unwrap();
        let sounds = scan_outputs(&mods, &D2R_SOUND_PACKS).unwrap();
        assert_eq!(sounds.len(), 2);
        assert!(sounds.keys().all(|k| k.starts_with("d2r-sounds-")));
    }

    #[test]
    fn importer_fingerprint_ignores_docs_tests_and_bytecode() {
        let temp = tempfile::tempdir().unwrap();
        let importer = temp.path();
        fs::create_dir_all(importer.join("d2r_import/__pycache__")).unwrap();
        fs::create_dir_all(importer.join("tests")).unwrap();
        fs::write(importer.join("d2r_import/__main__.py"), "print(1)").unwrap();
        fs::write(importer.join("mapping.toml"), "a = 1").unwrap();
        let before = importer_fingerprint(importer).unwrap();
        fs::write(importer.join("README.md"), "docs").unwrap();
        fs::write(importer.join("tests/test_x.py"), "x").unwrap();
        fs::write(importer.join("d2r_import/__pycache__/m.pyc"), "x").unwrap();
        assert_eq!(importer_fingerprint(importer).unwrap(), before);
        fs::write(importer.join("mapping.toml"), "a = 2").unwrap();
        assert_ne!(importer_fingerprint(importer).unwrap(), before);
    }

    #[test]
    fn clear_removes_only_launcher_outputs() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().canonicalize().unwrap();
        let cache = root.join("damnationx/d2r-models");
        fs::create_dir_all(cache.join("glb/env")).unwrap();
        fs::write(cache.join("glb/mapping.resolved.json"), b"{}").unwrap();
        fs::write(cache.join(ART_STAMP), b"{}").unwrap();
        fs::write(cache.join(SOUND_STAMP), b"{}").unwrap();
        // Siblings of the cache: HD library and the managed Python stay.
        fs::create_dir_all(root.join("damnationx/hd-combined/hd")).unwrap();
        fs::write(root.join("damnationx/hd-combined/hd/keep.png"), b"x").unwrap();
        let devilution = root.join("diasurgical/devilution");
        let mods = devilution.join("mods");
        for name in D2R_SOUND_PACKS {
            fs::create_dir_all(mods.join(name).join("sfx")).unwrap();
            fs::write(mods.join(name).join("sfx/a.wav"), b"RIFF").unwrap();
        }
        fs::create_dir_all(mods.join("d2sounds")).unwrap();
        fs::write(mods.join("d2sounds/keep.wav"), b"RIFF").unwrap();
        fs::write(devilution.join("single_0.sv"), b"save").unwrap();
        let owned = root.join("Diablo II Resurrected");
        fs::create_dir_all(owned.join("Data")).unwrap();
        fs::write(owned.join(".build.info"), b"x").unwrap();
        // A link inside the cache must not lead to deleting what it points at.
        #[cfg(unix)]
        std::os::unix::fs::symlink(&owned, cache.join("glb/owned-link")).unwrap();

        let removed = clear_import(&cache, &mods, &[owned.clone(), devilution.clone()]);
        // The saves folder contains the mods folder, so it cannot be protected
        // wholesale; the caller protects the install and data instead.
        assert!(removed.is_err());
        let removed = clear_import(&cache, &mods, &[owned.clone()]).unwrap();
        assert_eq!(removed.len(), 3);
        assert!(!cache.exists());
        for name in D2R_SOUND_PACKS {
            assert!(!mods.join(name).exists());
        }
        assert!(mods.join("d2sounds/keep.wav").is_file());
        assert!(devilution.join("single_0.sv").is_file());
        assert!(owned.join(".build.info").is_file());
        assert!(owned.join("Data").is_dir());
        assert!(root.join("damnationx/hd-combined/hd/keep.png").is_file());
        // Clearing again is a no-op.
        assert!(clear_import(&cache, &mods, &[owned.clone()])
            .unwrap()
            .is_empty());
    }

    #[test]
    fn clear_refuses_unmanaged_or_overlapping_folders() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().canonicalize().unwrap();
        let mods = root.join("mods");
        let other = root.join("my-converted-assets");
        fs::create_dir_all(&other).unwrap();
        assert!(clear_import(&other, &mods, &[]).is_err());
        assert!(other.is_dir());
        assert!(clear_import(Path::new("d2r-models"), &mods, &[]).is_err());
        // An install placed inside the cache is never deleted.
        let cache = root.join("d2r-models");
        let owned = cache.join("Diablo II Resurrected");
        fs::create_dir_all(&owned).unwrap();
        assert!(clear_import(&cache, &mods, &[owned.clone()]).is_err());
        assert!(owned.is_dir());
    }
}
