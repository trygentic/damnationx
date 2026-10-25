//! Private conversion work survives errors/cancellation, separate from playable
//! assets. The caller holds the installation lock for this entire lifecycle.
use crate::d2r_cache::{self, SourceFingerprint};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

const VERSION: u32 = 1;
const STATE: &str = "resume.json";

#[derive(Default, Serialize, Deserialize)]
struct Checkpoint {
    recipe: u64,
    /// None while a phase is running or interrupted; completed files remain
    /// available to the importer's individual model/texture caches.
    outputs: Option<BTreeMap<String, u64>>,
}

#[derive(Serialize, Deserialize)]
struct State {
    version: u32,
    source: SourceFingerprint,
    phases: BTreeMap<String, Checkpoint>,
}

pub struct Work {
    root: PathBuf,
    pub cache: PathBuf,
    pub continuing: bool,
    state: State,
}

impl Work {
    pub fn open(cache: &Path, game: &Path) -> Result<Self, String> {
        let root = cache.join(".import-work");
        let source = d2r_cache::source_fingerprint(game, 0)?;
        if root.exists() || fs::symlink_metadata(&root).is_ok() {
            // Refuse links, including inside retained raw extracts and helpers.
            d2r_cache::scan_outputs(&root, &[])?;
        }
        let previous = fs::read(root.join(STATE))
            .ok()
            .and_then(|bytes| serde_json::from_slice::<State>(&bytes).ok())
            .filter(|state| state.version == VERSION && state.source == source);
        let continuing = previous.is_some();
        if !continuing && root.exists() {
            fs::remove_dir_all(&root).map_err(|e| e.to_string())?;
        }
        let state = previous.unwrap_or(State {
            version: VERSION,
            source,
            phases: BTreeMap::new(),
        });
        let cache = root.join("content");
        fs::create_dir_all(&cache).map_err(|e| e.to_string())?;
        let work = Self {
            root,
            cache,
            continuing,
            state,
        };
        work.save()?;
        Ok(work)
    }

    fn save(&self) -> Result<(), String> {
        let temporary = self.root.join("resume.writing.json");
        fs::write(
            &temporary,
            serde_json::to_vec(&self.state).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
        fs::rename(&temporary, self.root.join(STATE)).map_err(|e| e.to_string())
    }

    fn owns(phase: &str, path: &str) -> bool {
        match phase {
            "models" => {
                !path.starts_with("env/") && !path.starts_with("ui/") && !path.starts_with("vfx/")
            }
            "environment" => path.starts_with("env/"),
            "ui" => path.starts_with("ui/"),
            "portal" => path.starts_with("vfx/"),
            _ => false,
        }
    }

    fn outputs(&self, phase: &str) -> Result<BTreeMap<String, u64>, String> {
        let glb = self.cache.join("glb");
        d2r_cache::scan_outputs(&glb, &[])?
            .into_keys()
            .filter(|path| Self::owns(phase, path))
            .map(|path| {
                Ok((
                    path.clone(),
                    d2r_cache::output_fingerprint(&glb.join(path))?,
                ))
            })
            .collect()
    }

    /// Skip only a completed, unchanged, validated phase. If its recipe changed
    /// or completed outputs were damaged, clear that component before retrying.
    pub fn begin(&mut self, phase: &str, importer: &Path, ready: bool) -> Result<bool, String> {
        // Everything else (CLI, shared storage, requirements, helpers and any
        // newly added recipes) conservatively invalidates the dependent phase.
        let independent: &[&str] = match phase {
            "models" => &[
                "d2r_import/env.py",
                "env_mapping.toml",
                "d2r_import/ui.py",
                "d2r_import/vfx.py",
            ],
            "environment" => &["mapping.toml", "d2r_import/ui.py", "d2r_import/vfx.py"],
            "ui" => &[
                "mapping.toml",
                "env_mapping.toml",
                "d2r_import/convert.py",
                "d2r_import/gltf.py",
                "d2r_import/curves.py",
                "d2r_import/granny.py",
                "d2r_import/env.py",
                "d2r_import/vfx.py",
            ],
            "portal" => &["mapping.toml", "env_mapping.toml", "d2r_import/ui.py"],
            _ => return Err(format!("Unknown import phase: {phase}")),
        };
        let recipe = d2r_cache::importer_fingerprint_excluding(importer, independent)?;
        let previous = self.state.phases.get(phase);
        let changed = previous.is_none_or(|previous| previous.recipe != recipe);
        let completed = previous.and_then(|previous| previous.outputs.as_ref());
        if !changed && ready {
            if let Some(outputs) = completed {
                if !outputs.is_empty() && *outputs == self.outputs(phase)? {
                    return Ok(true);
                }
            }
        }
        if changed || completed.is_some() {
            let glb = self.cache.join("glb");
            for path in d2r_cache::scan_outputs(&glb, &[])?
                .into_keys()
                .filter(|path| Self::owns(phase, path))
            {
                fs::remove_file(glb.join(path)).map_err(|e| e.to_string())?;
            }
        } else if phase == "models" {
            self.discard_interrupted_models()?;
        }
        self.state.phases.insert(
            phase.into(),
            Checkpoint {
                recipe,
                outputs: None,
            },
        );
        self.save()?;
        Ok(false)
    }

    pub fn complete(&mut self, phase: &str) -> Result<(), String> {
        let outputs = self.outputs(phase)?;
        self.state
            .phases
            .get_mut(phase)
            .ok_or("Phase was not started")?
            .outputs = Some(outputs);
        self.save()
    }

    fn discard_interrupted_models(&self) -> Result<(), String> {
        let glb = self.cache.join("glb");
        for name in d2r_cache::scan_outputs(&glb, &[])?
            .into_keys()
            .filter(|name| Self::owns("models", name) && name.ends_with(".glb"))
        {
            let model = glb.join(&name);
            let meta = model.with_extension("json");
            // The model writer finishes its sidecar after geometry and variant
            // textures. Missing/truncated sidecars cannot certify a partial GLB.
            let info = fs::metadata(&meta)
                .ok()
                .filter(|m| m.len() <= 16 * 1024 * 1024)
                .and_then(|_| fs::read(&meta).ok())
                .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(&bytes).ok());
            let valid = crate::assets::contained_file(&glb, &name).is_ok()
                && info.as_ref().is_some_and(|info| {
                    info["clips"].is_array()
                        && info.get("variants").is_none_or(|variants| {
                            variants.as_object().is_some_and(|variants| {
                                variants.values().all(|materials| {
                                    materials.as_object().is_some_and(|materials| {
                                        materials.values().all(|path| {
                                            path.as_str().is_some_and(|path| {
                                                crate::assets::contained_file(&glb, path).is_ok()
                                            })
                                        })
                                    })
                                })
                            })
                        })
                });
            if !valid {
                fs::remove_file(model).map_err(|e| e.to_string())?;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn model(root: &Path, name: &str) {
        let mut bytes = b"glTF".to_vec();
        bytes.extend(2u32.to_le_bytes());
        bytes.extend(12u32.to_le_bytes());
        fs::write(root.join(format!("{name}.glb")), bytes).unwrap();
        fs::write(root.join(format!("{name}.json")), br#"{"clips":[]}"#).unwrap();
    }

    #[test]
    fn interrupted_models_keep_complete_files_and_discard_incomplete_ones() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("owned");
        fs::create_dir_all(&game).unwrap();
        fs::write(game.join(".build.info"), "build 1").unwrap();
        let importer = temp.path().join("recipes");
        fs::create_dir_all(&importer).unwrap();
        fs::write(importer.join("mapping.toml"), "recipe 1").unwrap();
        let cache = temp.path().join("d2r-models");
        let mut work = Work::open(&cache, &game).unwrap();
        let glb = work.cache.join("glb");
        fs::create_dir(&glb).unwrap();
        assert!(!work.begin("models", &importer, false).unwrap());
        model(&glb, "good");
        model(&glb, "truncated");
        fs::write(glb.join("truncated.glb"), b"glTF").unwrap();
        model(&glb, "sidecar");
        fs::write(glb.join("sidecar.json"), b"{").unwrap();
        model(&glb, "variant");
        fs::write(
            glb.join("variant.json"),
            br#"{"clips":[],"variants":{"blue":{"body":"variants/missing.png"}}}"#,
        )
        .unwrap();
        drop(work); // Equivalent retained state after a failed/cancelled process.
        let mut work = Work::open(&cache, &game).unwrap();
        assert!(!work.begin("models", &importer, false).unwrap());
        assert!(glb.join("good.glb").exists());
        for name in ["truncated", "sidecar", "variant"] {
            assert!(!glb.join(format!("{name}.glb")).exists(), "{name}");
        }
    }

    #[test]
    fn completed_work_checks_contents_recipes_and_owned_build() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("owned");
        let importer = temp.path().join("recipes");
        fs::create_dir_all(importer.join("d2r_import")).unwrap();
        fs::create_dir_all(&game).unwrap();
        fs::write(game.join(".build.info"), "build 1").unwrap();
        let cache = temp.path().join("d2r-models");
        let mut work = Work::open(&cache, &game).unwrap();
        let glb = work.cache.join("glb");
        fs::create_dir(&glb).unwrap();
        work.begin("models", &importer, false).unwrap();
        model(&glb, "hero");
        work.complete("models").unwrap();
        fs::write(importer.join("d2r_import/vfx.py"), "fixed portal").unwrap();
        assert!(work.begin("models", &importer, true).unwrap());
        // Detect a same-size content change, beyond ordinary manifest checks.
        fs::write(glb.join("hero.json"), br#"{"clips":{}}"#).unwrap();
        assert!(!work.begin("models", &importer, true).unwrap());
        assert!(!glb.join("hero.glb").exists());
        model(&glb, "hero");
        work.complete("models").unwrap();
        fs::write(importer.join("mapping.toml"), "changed mapping").unwrap();
        assert!(!work.begin("models", &importer, true).unwrap());
        fs::write(work.cache.join("old-extract"), "old build").unwrap();
        drop(work);
        fs::write(game.join(".build.info"), "build 2").unwrap();
        let work = Work::open(&cache, &game).unwrap();
        assert!(!work.continuing);
        assert!(!work.cache.join("old-extract").exists());
    }

    #[cfg(unix)]
    #[test]
    fn retained_work_never_follows_links() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("owned");
        let cache = temp.path().join("d2r-models");
        fs::create_dir_all(&game).unwrap();
        fs::create_dir_all(&cache).unwrap();
        fs::write(game.join(".build.info"), "build 1").unwrap();
        std::os::unix::fs::symlink(&game, cache.join(".import-work")).unwrap();
        assert!(Work::open(&cache, &game).is_err());
        assert!(!game.join("resume.json").exists());
    }
}
