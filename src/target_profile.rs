use target_lexicon::{Architecture, BinaryFormat, Triple};

#[derive(Debug, Clone, PartialEq)]
pub enum LldFlavor {
    Gnu,
    Darwin,
    Coff,
}

#[derive(Debug, Clone)]
pub enum OutputFormat {
    Elf,
    MachO,
    Pe,
}

#[derive(Debug, Clone)]
pub struct TargetProfile {
    pub triple: Triple,
    pub lld_flavor: LldFlavor,
    pub output_format: OutputFormat,
    pub entry_symbol: &'static str,
    pub default_output_extension: &'static str,
    pub static_link_flags: Vec<&'static str>,
    pub lib_prefix: &'static str,
    pub lib_extension: &'static str,
}

impl TargetProfile {
    pub fn from_triple(triple: Triple) -> Self {
        let (lld_flavor, output_format, entry_symbol, ext, static_flags, lib_prefix, lib_extension) =
            match triple.binary_format {
                BinaryFormat::Elf => {
                    let arch_flags = match triple.architecture {
                        Architecture::X86_64 => vec!["-static"],
                        Architecture::Aarch64(_) => vec!["-static"],
                        _ => vec!["-static"],
                    };
                    (
                        LldFlavor::Gnu,
                        OutputFormat::Elf,
                        "_start",
                        "out",
                        arch_flags,
                        "lib",
                        ".so",
                    )
                }
                BinaryFormat::Macho => (
                    LldFlavor::Darwin,
                    OutputFormat::MachO,
                    "_main",
                    "out",
                    vec!["-static", "-no_pie"],
                    "lib",
                    ".dylib",
                ),
                BinaryFormat::Coff => (
                    LldFlavor::Coff,
                    OutputFormat::Pe,
                    "mainCRTStartup",
                    "exe",
                    vec!["/NODEFAULTLIB", "/SUBSYSTEM:CONSOLE"],
                    "",
                    ".dll",
                ),
                _ => {
                    let arch_flags = match triple.architecture {
                        Architecture::X86_64 => vec!["-static"],
                        Architecture::Aarch64(_) => vec!["-static"],
                        _ => vec!["-static"],
                    };
                    (
                        LldFlavor::Gnu,
                        OutputFormat::Elf,
                        "_start",
                        "out",
                        arch_flags,
                        "lib",
                        ".so",
                    )
                }
            };

        TargetProfile {
            triple,
            lld_flavor,
            output_format,
            entry_symbol,
            default_output_extension: ext,
            static_link_flags: static_flags,
            lib_prefix,
            lib_extension,
        }
    }

    pub fn host() -> Self {
        Self::from_triple(Triple::host())
    }

    pub fn lld_binary_name(&self) -> &'static str {
        match self.lld_flavor {
            LldFlavor::Gnu => "ld.lld",
            LldFlavor::Darwin => "ld64.lld",
            LldFlavor::Coff => "lld-link",
        }
    }

    pub fn is_static_by_default(&self) -> bool {
        true
    }

    pub fn format_lib_name(&self, lib_name: &str) -> String {
        if self.lld_flavor == LldFlavor::Coff {
            if lib_name.ends_with(".lib") {
                lib_name.to_string()
            } else {
                format!("{}.lib", lib_name)
            }
        } else {
            if lib_name.starts_with(self.lib_prefix) {
                lib_name[self.lib_prefix.len()..].to_string()
            } else {
                lib_name.to_string()
            }
        }
    }
}