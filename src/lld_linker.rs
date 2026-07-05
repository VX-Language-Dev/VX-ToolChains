use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;

use crate::target_profile::TargetProfile;

#[derive(Debug)]
pub enum LldLinkerError {
    LldNotFound,
    LinkFailed { stderr: String },
    Io(std::io::Error),
    ObjectFileWriteFailed,
}

impl std::fmt::Display for LldLinkerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LldLinkerError::LldNotFound => write!(f, "LLD linker not found. Please install LLVM/LLD and ensure ld.lld/lld-link/ld64.lld is in PATH."),
            LldLinkerError::LinkFailed { stderr } => write!(f, "LLD link failed: {}", stderr),
            LldLinkerError::Io(e) => write!(f, "IO error: {}", e),
            LldLinkerError::ObjectFileWriteFailed => write!(f, "Failed to write temporary object file"),
        }
    }
}

impl std::error::Error for LldLinkerError {}

pub struct LldLinker;

impl LldLinker {
    pub fn link(
        obj_data: &[u8],
        output_path: &str,
        external_deps: &[String],
        target: &TargetProfile,
    ) -> Result<(), LldLinkerError> {
        let lld_path = Self::find_lld(target)?;

        let temp_dir = env::temp_dir();
        let obj_file_path = temp_dir.join(format!("vx_lld_{}.o", std::process::id()));
        fs::write(&obj_file_path, obj_data).map_err(|_| LldLinkerError::ObjectFileWriteFailed)?;

        let mut cmd = Command::new(&lld_path);

        cmd.arg("-o").arg(output_path);

        for flag in &target.static_link_flags {
            cmd.arg(flag);
        }

        cmd.arg("-e").arg(target.entry_symbol);

        for dep in external_deps {
            let lib_name = target.format_lib_name(dep);
            if target.lld_flavor == crate::target_profile::LldFlavor::Coff {
                cmd.arg(&lib_name);
            } else {
                cmd.arg(format!("-l{}", lib_name));
            }
        }

        cmd.arg(&obj_file_path);

        let output = cmd.output().map_err(LldLinkerError::Io)?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr).to_string();
            return Err(LldLinkerError::LinkFailed { stderr });
        }

        #[cfg(unix)]
        {
            let metadata = fs::metadata(output_path).map_err(LldLinkerError::Io)?;
            let mut perms = metadata.permissions();
            perms.set_mode(0o755);
            fs::set_permissions(output_path, perms).map_err(LldLinkerError::Io)?;
        }

        let _ = fs::remove_file(obj_file_path);

        Ok(())
    }

    fn find_lld(target: &TargetProfile) -> Result<PathBuf, LldLinkerError> {
        let candidates = match target.lld_flavor {
            crate::target_profile::LldFlavor::Gnu => vec!["ld.lld", "ld64.lld", "lld"],
            crate::target_profile::LldFlavor::Darwin => vec!["ld64.lld", "ld.lld", "lld"],
            crate::target_profile::LldFlavor::Coff => vec!["lld-link", "ld.lld", "lld"],
        };

        for name in candidates {
            if let Ok(path) = env::var("LLD_PATH") {
                let p = Path::new(&path).join(name);
                if p.exists() {
                    return Ok(p);
                }
            }

            if let Some(path) = which::which(name).ok() {
                return Ok(path);
            }
        }

        Err(LldLinkerError::LldNotFound)
    }
}