mod audio;
mod dsp;
mod params;
mod ws;

use audio::PitchEvent;
use params::AtomicParams;
use std::sync::Arc;
use tauri::{
    menu::{Menu, MenuItem},
    tray::TrayIconBuilder,
};
use tokio::sync::broadcast;

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let params = Arc::new(AtomicParams::default());
    let (pitch_tx, _) = broadcast::channel::<PitchEvent>(64);

    // Bridge crossbeam (audio thread) → tokio broadcast (WS task)
    let (cb_tx, cb_rx) = crossbeam_channel::bounded::<PitchEvent>(64);
    let pitch_tx_clone = pitch_tx.clone();
    std::thread::Builder::new()
        .name("pitch-bridge".into())
        .spawn(move || {
            for ev in cb_rx {
                let _ = pitch_tx_clone.send(ev);
            }
        })
        .unwrap();

    // Audio engine
    audio::start(params.clone(), cb_tx);

    // Tokio runtime for WebSocket server
    let rt = tokio::runtime::Runtime::new().unwrap();
    let params_ws = params.clone();
    let pitch_rx = pitch_tx.subscribe();
    rt.spawn(async move {
        ws::run(params_ws, pitch_rx).await;
    });

    // Tauri tray app (no window)
    tauri::Builder::default()
        .setup(move |app| {
            // Prevent dock icon on macOS
            #[cfg(target_os = "macos")]
            app.set_activation_policy(tauri::ActivationPolicy::Accessory);

            let quit = MenuItem::with_id(app, "quit", "Quit Silvertune Companion", true, None::<&str>)?;
            let open = MenuItem::with_id(app, "open", "Open silvertune.live", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&open, &quit])?;

            TrayIconBuilder::new()
                .menu(&menu)
                .tooltip("Silvertune Companion — running")
                .icon(app.default_window_icon().unwrap().clone())
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "quit" => app.exit(0),
                    "open" => {
                        let _ = open::that("https://silvertune.live");
                    }
                    _ => {}
                })
                .build(app)?;

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("tauri error");
}
