use std::process::Command;

pub fn run_command(cmd: String) {
    std::thread::spawn(move || match Command::new("sh").arg("-c").arg(&cmd).status() {
        Ok(status) if !status.success() => {
            eprintln!("[warn] command exited with {}: {}", status, cmd);
        }
        Err(e) => {
            eprintln!("[error] failed to run `{}`: {}", cmd, e);
        }
        _ => {}
    });
}
