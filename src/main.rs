mod args;
mod command;
mod wayland;

use args::Args;
use command::run_command;
use wayland::State;
use wayland_client::{Connection, EventQueue};

fn main() {
    let args = Args::parse();

    if args.timeouts.is_empty() {
        eprintln!("[error] no --timeout rules given, nothing to do");
        std::process::exit(1);
    }

    for rule in &args.timeouts {
        println!("[config] after {}s -> `{}`", rule.seconds, rule.command);
    }
    if let Some(ref cmd) = args.resume_cmd {
        println!("[config] on resume -> `{}`", cmd);
    }

    let conn = Connection::connect_to_env().expect("failed to connect to wayland display");
    let display = conn.display();

    let mut event_queue: EventQueue<State> = conn.new_event_queue();
    let qh = event_queue.handle();

    let _registry = display.get_registry(&qh, ());

    let resume_cmd = args.resume_cmd;
    let mut state = State {
        notifier: None,
        seat: None,
        on_idle: Some(Box::new(|cmd| {
            run_command(cmd.to_owned());
        })),
        on_resume: resume_cmd.map(|cmd| {
            Box::new(move || {
                println!("[event] RESUMED -> running: {}", cmd);
                run_command(cmd.clone());
            }) as Box<dyn FnMut()>
        }),
    };

    event_queue.roundtrip(&mut state).unwrap();

    let notifier = state
        .notifier
        .as_ref()
        .expect("compositor does not support ext-idle-notify-v1");
    let seat = state.seat.as_ref().expect("no wl_seat found");

    let mut _notifications = Vec::new();
    for rule in &args.timeouts {
        println!(
            "[init] registering timeout: {}ms -> `{}`",
            rule.seconds * 1000,
            rule.command
        );
        let notification = notifier.get_idle_notification(
            rule.seconds * 1000,
            seat,
            &qh,
            rule.command.clone(),
        );
        _notifications.push(notification);
    }

    println!("[init] listening for idle/resume events... (Ctrl+C to quit)");
    loop {
        event_queue.blocking_dispatch(&mut state).unwrap();
    }
}
