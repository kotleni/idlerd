use clap::Parser;

#[derive(Parser, Debug)]
#[command(name = "idlerd", about = "A wlroots idle daemon")]
pub struct RawArgs {
    /// Repeatable: --timeout <seconds> <command>
    #[arg(long = "timeout", num_args = 2, value_names = ["SECONDS", "COMMAND"])]
    pub timeout: Vec<String>,

    /// Command to run when resuming from idle
    #[arg(long = "resume", value_name = "COMMAND")]
    pub resume: Option<String>,
}

#[derive(Debug, Clone)]
pub struct TimeoutRule {
    pub seconds: u32,
    pub command: String,
}

#[derive(Debug)]
pub struct Args {
    pub timeouts: Vec<TimeoutRule>,
    pub resume_cmd: Option<String>,
}

impl Args {
    pub fn parse() -> Self {
        let raw = RawArgs::parse();

        let timeouts = raw
            .timeout
            .chunks(2)
            .map(|pair| {
                let seconds: u32 = pair[0]
                    .parse()
                    .unwrap_or_else(|_| panic!("invalid timeout seconds: {}", pair[0]));
                TimeoutRule {
                    seconds,
                    command: pair[1].clone(),
                }
            })
            .collect();

        Args {
            timeouts,
            resume_cmd: raw.resume,
        }
    }
}
