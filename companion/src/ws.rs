use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use std::sync::atomic::Ordering;
use tokio::net::TcpListener;
use tokio::sync::broadcast;
use tokio_tungstenite::accept_async;
use tokio_tungstenite::tungstenite::Message;

use crate::audio::PitchEvent;
use crate::params::SharedParams;

#[derive(Serialize)]
#[serde(tag = "type", rename_all = "camelCase")]
enum OutMsg {
    #[serde(rename = "pitch")]
    Pitch {
        #[serde(rename = "detectedNote")]
        detected_note: i32,
        #[serde(rename = "correctedNote")]
        corrected_note: i32,
        rms: f32,
    },
    #[serde(rename = "ping")]
    Ping,
}

#[derive(Deserialize)]
struct ParamsMsg {
    key:   Option<u8>,
    scale: Option<u8>,
    tune:  Option<f32>,
    wide:  Option<f32>,
}

pub async fn run(params: SharedParams, mut pitch_rx: broadcast::Receiver<PitchEvent>) {
    let listener = TcpListener::bind("127.0.0.1:2747")
        .await
        .expect("bind :2747");
    log::info!("WebSocket server listening on ws://127.0.0.1:2747");

    loop {
        let (stream, addr) = match listener.accept().await {
            Ok(v) => v,
            Err(e) => { log::warn!("accept error: {e}"); continue; }
        };
        log::info!("client connected: {addr}");
        let p = params.clone();
        let rx = pitch_rx.resubscribe();
        tokio::spawn(handle_client(stream, p, rx));
    }
}

async fn handle_client(
    stream: tokio::net::TcpStream,
    params: SharedParams,
    mut pitch_rx: broadcast::Receiver<PitchEvent>,
) {
    let ws = match accept_async(stream).await {
        Ok(ws) => ws,
        Err(e) => { log::warn!("ws handshake: {e}"); return; }
    };
    let (mut sink, mut source) = ws.split();

    // Heartbeat every 2s
    let mut heartbeat = tokio::time::interval(std::time::Duration::from_secs(2));

    loop {
        tokio::select! {
            msg = source.next() => {
                match msg {
                    Some(Ok(Message::Text(txt))) => {
                        if let Ok(p) = serde_json::from_str::<serde_json::Value>(&txt) {
                            if p.get("type").and_then(|t| t.as_str()) == Some("params") {
                                if let Ok(pm) = serde_json::from_value::<ParamsMsg>(p) {
                                    if let Some(k) = pm.key   { params.key.store(k as u32, Ordering::Relaxed); }
                                    if let Some(s) = pm.scale { params.scale.store(s as u32, Ordering::Relaxed); }
                                    if let Some(t) = pm.tune  { params.set_tune(t); }
                                    if let Some(w) = pm.wide  { params.set_wide(w); }
                                }
                            }
                        }
                    }
                    Some(Ok(Message::Close(_))) | None => break,
                    _ => {}
                }
            }
            ev = pitch_rx.recv() => {
                match ev {
                    Ok(e) => {
                        let msg = OutMsg::Pitch {
                            detected_note: e.detected_note,
                            corrected_note: e.corrected_note,
                            rms: e.rms,
                        };
                        let json = serde_json::to_string(&msg).unwrap();
                        if sink.send(Message::Text(json.into())).await.is_err() { break; }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => continue,
                    Err(_) => break,
                }
            }
            _ = heartbeat.tick() => {
                let json = serde_json::to_string(&OutMsg::Ping).unwrap();
                if sink.send(Message::Text(json.into())).await.is_err() { break; }
            }
        }
    }
    log::info!("client disconnected");
}
