mod homekit;
mod rpitx;

use std::collections::HashMap;
use std::sync::Arc;

use futures_util::{SinkExt, StreamExt};
use reqwest::Client;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tokio::sync::Mutex;
use tokio::time::{sleep, Duration};
use tokio_tungstenite::{connect_async, tungstenite::Message};
use tracing::{error, info, warn};

// ── Config ────────────────────────────────────────────────────────────────────

#[derive(Deserialize)]
struct Config {
    #[serde(default)]
    homeassistant: Option<HaConfig>,
    fans: HashMap<String, String>, // room → 14-bit fan ID string
    #[serde(default)]
    homekit: Option<HomeKitConfig>,
}

#[derive(Deserialize)]
struct HaConfig {
    #[serde(default = "default_true")]
    enabled: bool,
    url: String,
    token: String,
}

fn default_true() -> bool { true }

#[derive(Deserialize)]
struct HomeKitConfig {
    #[serde(default)]
    enabled: bool,
    #[serde(default = "default_hk_port")]
    port: u16,
    #[serde(default = "default_hk_persist")]
    persist_dir: String,
    #[serde(default = "default_hk_pin")]
    pin: String,  // 8 digits, e.g. "03141592"
    #[serde(default)]
    name: Option<String>,
}

fn default_hk_port() -> u16 { 51826 }
fn default_hk_persist() -> String { "homekit".into() }
fn default_hk_pin() -> String { "03141592".into() }

fn hostname() -> String {
    std::fs::read_to_string("/etc/hostname")
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "onlyfansd".into())
}

fn parse_pin(s: &str) -> [u8; 8] {
    let digits: Vec<u8> = s.chars().filter_map(|c| c.to_digit(10).map(|d| d as u8)).collect();
    let mut out = [0u8; 8];
    for (i, d) in digits.iter().take(8).enumerate() { out[i] = *d; }
    out
}

// ── State ─────────────────────────────────────────────────────────────────────

#[derive(Serialize, Deserialize, Clone, Debug)]
struct FanState {
    state: String,
    percentage: u32,
    #[serde(default = "default_direction")]
    direction: String,
}

fn default_direction() -> String {
    "forward".to_string()
}

impl Default for FanState {
    fn default() -> Self {
        Self { state: "OFF".into(), percentage: 0, direction: "forward".into() }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug)]
struct LightState {
    state: String,
}

impl Default for LightState {
    fn default() -> Self {
        Self { state: "OFF".into() }
    }
}

#[derive(Serialize, Deserialize, Default)]
struct PersistedState {
    fans: HashMap<String, FanState>,
    lights: HashMap<String, LightState>,
}

struct SharedState {
    fan_states: HashMap<String, FanState>,
    light_states: HashMap<String, LightState>,
    pending_timers: HashMap<String, tokio::task::AbortHandle>,
}

impl SharedState {
    fn new() -> Self {
        Self { fan_states: HashMap::new(), light_states: HashMap::new(), pending_timers: HashMap::new() }
    }
}

// ── RF Protocol ───────────────────────────────────────────────────────────────

// 304.30 MHz OOK, 333 µs per chip. Each bit → 3 chips '10b'.
// Message = preamble(4) + FAN_ID(14) + cmdid(7) = 25 bits = 75 chips + 30-chip pause.
const CMDS: &[(&str, &str)] = &[
    ("reverse", "0001000"),
    ("light",   "0000010"),
    ("stop",    "0000100"),
    ("speed1",  "0010000"),
    ("speed2",  "0010100"),
    ("speed3",  "0100000"),
    ("speed4",  "0110000"),
    ("speed5",  "1000100"),
    ("speed6",  "1000000"),
    ("pair",    "0100100"),
];

fn build_chip_stream(fan_id: &str, cmd: &str) -> Option<String> {
    assert_eq!(fan_id.len(), 14, "FAN_ID must be 14 bits, got {}", fan_id.len());
    let cmd_bits = CMDS.iter().find(|(k, _)| *k == cmd)?.1;
    let bits = format!("1111{}{}", fan_id, cmd_bits);
    let chips: String = bits.chars().map(|b| format!("10{}", b)).collect();
    Some(format!("{}{}", chips, "0".repeat(30)))
}

// 304.30 MHz, 333 µs per chip, 3 repeats with 1 ms pause — matches the
// upstream remote and what the previous `sendook` invocation did.
const RF_FREQ_HZ: u64 = 304_300_000;
const RF_CHIP_US: u32 = 333;
const RF_REPEAT: u32 = 3;
const RF_PAUSE_US: u32 = 1_000;

async fn send_rf(fan_id: &str, cmd: &str) {
    let chips = match build_chip_stream(fan_id, cmd) {
        Some(c) => c,
        None => { error!("Unknown RF command: {}", cmd); return; }
    };
    info!("rpitx send_ook freq={} chips={} repeats={}", RF_FREQ_HZ, chips.len(), RF_REPEAT);

    // librpitx grabs DMA channels + maps /dev/mem; running it on the tokio
    // worker thread would block the runtime for the full transmission
    // (~30 ms per repeat). Run on a blocking thread instead.
    let chips_owned = chips;
    let res = tokio::task::spawn_blocking(move || {
        rpitx::send_ook(RF_FREQ_HZ, &chips_owned, RF_CHIP_US, RF_REPEAT, RF_PAUSE_US)
    })
    .await;

    match res {
        Ok(Ok(())) => {}
        Ok(Err(e)) => error!("rpitx send_ook failed: {}", e),
        Err(e) => error!("rpitx send_ook task panicked: {}", e),
    }
}

// ── Persistence ───────────────────────────────────────────────────────────────

const STATE_FILE: &str = "fan_state.json";

fn save_state(s: &SharedState) {
    let p = PersistedState { fans: s.fan_states.clone(), lights: s.light_states.clone() };
    if let Ok(json) = serde_json::to_string_pretty(&p) {
        if let Err(e) = std::fs::write(STATE_FILE, json) {
            error!("save state: {}", e);
        }
    }
}

fn load_state() -> PersistedState {
    match std::fs::read_to_string(STATE_FILE) {
        Ok(s) => serde_json::from_str(&s).unwrap_or_default(),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => PersistedState::default(),
        Err(e) => { error!("load state: {}", e); PersistedState::default() }
    }
}

// ── Home Assistant REST ───────────────────────────────────────────────────────

async fn ha_set_state(client: &Client, url: &str, token: &str, entity_id: &str, state: &str, attrs: Value) {
    let endpoint = format!("{}/api/states/{}", url, entity_id);
    match client.post(&endpoint)
        .header("Authorization", format!("Bearer {}", token))
        .json(&json!({ "state": state, "attributes": attrs }))
        .timeout(Duration::from_secs(5))
        .send()
        .await
    {
        Ok(r) if r.status().is_success() => {}
        Ok(r) => error!("HA {} → {}", entity_id, r.status()),
        Err(e) => error!("HA {} failed: {}", entity_id, e),
    }
}

fn title_case(s: &str) -> String {
    s.split('_')
        .map(|w| {
            let mut c = w.chars();
            c.next().map(|f| f.to_uppercase().collect::<String>() + c.as_str()).unwrap_or_default()
        })
        .collect::<Vec<_>>()
        .join(" ")
}

// ── Daemon ────────────────────────────────────────────────────────────────────

struct Daemon {
    config: Config,
    client: Client,
    state: Mutex<SharedState>,
    homekit: Option<Arc<homekit::HomeKit>>,
}

impl Daemon {
    fn new(config: Config) -> Self {
        let persisted = load_state();
        info!("Loaded persisted state");
        let mut state = SharedState::new();
        state.fan_states = persisted.fans;
        state.light_states = persisted.lights;
        Self { config, client: Client::new(), state: Mutex::new(state), homekit: None }
    }

    /// Returns the HA config only when it's present AND enabled.
    fn ha(&self) -> Option<&HaConfig> {
        self.config.homeassistant.as_ref().filter(|h| h.enabled)
    }

    async fn sync_fan_to_ha(&self, room: &str, st: &FanState) {
        let Some(ha) = self.ha() else { return; };
        ha_set_state(
            &self.client,
            &ha.url,
            &ha.token,
            &format!("fan.{}_fan", room),
            &st.state.to_lowercase(),
            json!({
                "percentage": st.percentage,
                "direction": st.direction,
                "supported_features": 5,
                "friendly_name": format!("{} Fan", title_case(room)),
            }),
        ).await;
    }

    async fn sync_light_to_ha(&self, room: &str, st: &LightState) {
        let Some(ha) = self.ha() else { return; };
        ha_set_state(
            &self.client,
            &ha.url,
            &ha.token,
            &format!("light.{}_fan_light", room),
            &st.state.to_lowercase(),
            json!({ "friendly_name": format!("{} Fan Light", title_case(room)) }),
        ).await;
    }

    async fn init_entities(&self) {
        if self.ha().is_none() {
            info!("Home Assistant integration disabled — skipping entity init");
            return;
        }
        info!("Initialising entities in Home Assistant...");
        for room in self.config.fans.keys() {
            let (fan_st, light_st) = {
                let s = self.state.lock().await;
                (s.fan_states.get(room).cloned().unwrap_or_default(),
                 s.light_states.get(room).cloned().unwrap_or_default())
            };
            self.sync_fan_to_ha(room, &fan_st).await;
            self.sync_light_to_ha(room, &light_st).await;
        }
        info!("Entities initialised");
    }

    async fn set_fan(&self, room: &str, new: FanState) {
        {
            let mut s = self.state.lock().await;
            s.fan_states.insert(room.to_string(), new.clone());
            save_state(&s);
        }
        self.sync_fan_to_ha(room, &new).await;
        if let Some(hk) = &self.homekit {
            hk.update_fan(
                room,
                new.state == "ON",
                new.percentage.min(100) as u8,
                new.direction != "reverse",
            ).await;
        }
    }

    async fn set_light(&self, room: &str, new: LightState) {
        {
            let mut s = self.state.lock().await;
            s.light_states.insert(room.to_string(), new.clone());
            save_state(&s);
        }
        self.sync_light_to_ha(room, &new).await;
        if let Some(hk) = &self.homekit {
            hk.update_light(room, new.state == "ON").await;
        }
    }

    // Maps HA percentage → fan speed index (0=fastest/speed1, 5=slowest/speed6, 6=stop).
    // Inverted because HA 100% = max speed, but speed1 is physically the fastest.
    fn speed_snap(percentage: u32) -> u32 {
        ((100u32.saturating_sub(percentage) as f64 / 100.0) * 6.0).round() as u32
    }

    async fn apply_fan_speed(&self, room: &str, percentage: u32) {
        let fan_id = match self.config.fans.get(room) {
            Some(id) => id.clone(),
            None => { error!("Unknown room: {}", room); return; }
        };
        let direction = {
            let s = self.state.lock().await;
            s.fan_states.get(room).map(|f| f.direction.clone()).unwrap_or_else(|| "forward".into())
        };
        let snapped = Self::speed_snap(percentage);
        if snapped >= 6 {
            send_rf(&fan_id, "stop").await;
            self.set_fan(room, FanState { state: "OFF".into(), percentage: 0, direction }).await;
        } else {
            let speed = format!("speed{}", snapped + 1);
            let snapped_pct = (100.0 - snapped as f64 * 100.0 / 6.0).round() as u32;
            info!("{} → {} ({}%)", room, speed, snapped_pct);
            send_rf(&fan_id, &speed).await;
            self.set_fan(room, FanState { state: "ON".into(), percentage: snapped_pct, direction }).await;
        }
    }

    async fn schedule_speed(self: Arc<Self>, room: String, percentage: u32) {
        // Immediate optimistic feedback to HA; RF transmission is debounced 1.5 s.
        let snapped = Self::speed_snap(percentage);
        let direction = {
            let s = self.state.lock().await;
            s.fan_states.get(&room).map(|f| f.direction.clone()).unwrap_or_else(|| "forward".into())
        };
        let imm = if snapped >= 6 {
            FanState { state: "OFF".into(), percentage: 0, direction }
        } else {
            let pct = (100.0 - snapped as f64 * 100.0 / 6.0).round() as u32;
            FanState { state: "ON".into(), percentage: pct, direction }
        };
        self.set_fan(&room, imm).await;

        {
            let mut s = self.state.lock().await;
            if let Some(h) = s.pending_timers.remove(&room) {
                h.abort();
            }
        }

        let d = Arc::clone(&self);
        let r = room.clone();
        let handle = tokio::spawn(async move {
            sleep(Duration::from_millis(1500)).await;
            d.apply_fan_speed(&r, percentage).await;
        });
        self.state.lock().await.pending_timers.insert(room, handle.abort_handle());
    }

    async fn handle_service(
        self: Arc<Self>,
        domain: String,
        service: String,
        entity_ids: Vec<String>,
        service_data: Value,
    ) {
        for entity_id in &entity_ids {
            if domain == "fan" {
                let room = match entity_id
                    .strip_prefix("fan.")
                    .and_then(|s| s.strip_suffix("_fan"))
                {
                    Some(r) if self.config.fans.contains_key(r) => r.to_string(),
                    _ => continue,
                };
                info!("fan {} {} {:?}", room, service, service_data);
                match service.as_str() {
                    "turn_on" => {
                        let pct = service_data["percentage"].as_u64().unwrap_or(35) as u32;
                        Arc::clone(&self).schedule_speed(room, pct).await;
                    }
                    "turn_off" => {
                        Arc::clone(&self).schedule_speed(room, 0).await;
                    }
                    "set_percentage" => {
                        let pct = service_data["percentage"].as_u64().unwrap_or(0) as u32;
                        Arc::clone(&self).schedule_speed(room, pct).await;
                    }
                    "set_direction" => {
                        if let Some(fan_id) = self.config.fans.get(&room).cloned() {
                            send_rf(&fan_id, "reverse").await;
                        }
                        let dir = service_data["direction"]
                            .as_str()
                            .unwrap_or("forward")
                            .to_string();
                        let current = self.state.lock().await
                            .fan_states.get(&room).cloned().unwrap_or_default();
                        self.set_fan(&room, FanState { direction: dir, ..current }).await;
                    }
                    _ => {}
                }
            } else if domain == "light" {
                let room = match entity_id
                    .strip_prefix("light.")
                    .and_then(|s| s.strip_suffix("_fan_light"))
                {
                    Some(r) if self.config.fans.contains_key(r) => r.to_string(),
                    _ => continue,
                };
                info!("light {} {}", room, service);
                if matches!(service.as_str(), "turn_on" | "turn_off" | "toggle") {
                    if let Some(fan_id) = self.config.fans.get(&room).cloned() {
                        send_rf(&fan_id, "light").await;
                    }
                    let current_on = self.state.lock().await
                        .light_states.get(&room)
                        .map(|l| l.state == "ON")
                        .unwrap_or(false);
                    let new_state = match service.as_str() {
                        "turn_on" => "ON",
                        "turn_off" => "OFF",
                        _ => if current_on { "OFF" } else { "ON" },
                    };
                    self.set_light(&room, LightState { state: new_state.into() }).await;
                }
            }
        }
    }

    async fn run(self: Arc<Self>) {
        let Some(ha) = self.ha() else {
            info!("Home Assistant integration disabled — daemon idle (HomeKit only)");
            std::future::pending::<()>().await;
            return;
        };
        let ws_url = ha.url
            .replace("https://", "wss://")
            .replace("http://", "ws://")
            + "/api/websocket";
        info!("Connecting to {}", ws_url);

        loop {
            match connect_async(&ws_url).await {
                Ok((stream, _)) => {
                    let (mut tx, mut rx) = stream.split();
                    let mut msg_id = 1u64;

                    while let Some(item) = rx.next().await {
                        match item {
                            Ok(Message::Text(txt)) => {
                                let data: Value = match serde_json::from_str(&txt) {
                                    Ok(v) => v,
                                    Err(e) => { warn!("JSON parse: {}", e); continue; }
                                };
                                match data["type"].as_str() {
                                    Some("auth_required") => {
                                        let _ = tx.send(Message::Text(json!({
                                            "type": "auth",
                                            "access_token": ha.token,
                                        }).to_string())).await;
                                    }
                                    Some("auth_ok") => {
                                        info!("Authenticated with Home Assistant WebSocket");
                                        msg_id += 1;
                                        let _ = tx.send(Message::Text(json!({
                                            "id": msg_id,
                                            "type": "subscribe_events",
                                            "event_type": "call_service",
                                        }).to_string())).await;
                                    }
                                    Some("auth_invalid") => {
                                        error!("Authentication failed — check your token");
                                        break;
                                    }
                                    Some("event") => {
                                        let ed = &data["event"]["data"];
                                        let domain = ed["domain"].as_str().unwrap_or("").to_string();
                                        let service = ed["service"].as_str().unwrap_or("").to_string();
                                        let sd = ed.get("service_data").cloned().unwrap_or(json!({}));

                                        // HA 2022+ uses target.entity_id; older uses service_data.entity_id
                                        let raw_ids = ed.get("target")
                                            .and_then(|t| t.get("entity_id"))
                                            .or_else(|| sd.get("entity_id"));
                                        let entity_ids: Vec<String> = match raw_ids {
                                            Some(Value::String(s)) => vec![s.clone()],
                                            Some(Value::Array(a)) => a.iter()
                                                .filter_map(|v| v.as_str().map(String::from))
                                                .collect(),
                                            _ => vec![],
                                        };

                                        if (domain == "fan" || domain == "light") && !entity_ids.is_empty() {
                                            let d = Arc::clone(&self);
                                            tokio::spawn(async move {
                                                d.handle_service(domain, service, entity_ids, sd).await;
                                            });
                                        }
                                    }
                                    Some("result") if data["success"] == false => {
                                        warn!("WS command failed: {}", data);
                                    }
                                    _ => {}
                                }
                            }
                            Ok(Message::Close(_)) => break,
                            Err(e) => { error!("WS error: {}", e); break; }
                            _ => {}
                        }
                    }
                }
                Err(e) => error!("WS connect failed: {}", e),
            }
            info!("Reconnecting in 10 s...");
            sleep(Duration::from_secs(10)).await;
        }
    }
}

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info,hap=debug,libmdns=error")),
        )
        .init();

    let config_path = std::env::args().nth(1).unwrap_or_else(|| "config.yaml".into());
    let raw = std::fs::read_to_string(&config_path)
        .unwrap_or_else(|e| panic!("Cannot read {}: {}", config_path, e));
    let config: Config = serde_yaml::from_str(&raw)
        .unwrap_or_else(|e| panic!("Cannot parse {}: {}", config_path, e));

    let mut daemon = Daemon::new(config);

    // Optional HomeKit bridge
    if let Some(hk_cfg) = daemon.config.homekit.as_ref().filter(|c| c.enabled) {
        let rooms: Vec<String> = daemon.config.fans.keys().cloned().collect();
        let pin_bytes = parse_pin(&hk_cfg.pin);
        let bridge_name = hk_cfg.name.clone().unwrap_or_else(hostname);
        match homekit::HomeKit::new(&bridge_name, &rooms, hk_cfg.port, &hk_cfg.persist_dir, pin_bytes).await {
            Ok((hk, server)) => {
                info!("HomeKit bridge \"{}\" on port {} (persist={})", bridge_name, hk_cfg.port, hk_cfg.persist_dir);
                daemon.homekit = Some(hk);
                tokio::spawn(homekit::run_server(server));
            }
            Err(e) => error!("HomeKit bridge failed to start: {}", e),
        }
    }

    let daemon = Arc::new(daemon);

    // If HomeKit is enabled, consume its command stream and dispatch as service calls.
    if let Some(hk) = daemon.homekit.clone() {
        let d = Arc::clone(&daemon);
        tokio::spawn(async move {
            let mut rx = match hk.cmd_rx.lock().await.take() {
                Some(r) => r,
                None => return,
            };
            while let Some(cmd) = rx.recv().await {
                info!("HomeKit → {} {} {} {}", cmd.domain, cmd.service, cmd.room, cmd.data);
                let entity = match cmd.domain {
                    "fan" => format!("fan.{}_fan", cmd.room),
                    "light" => format!("light.{}_fan_light", cmd.room),
                    _ => continue,
                };
                Arc::clone(&d).handle_service(
                    cmd.domain.to_string(),
                    cmd.service.to_string(),
                    vec![entity],
                    cmd.data,
                ).await;
            }
        });
    }

    daemon.init_entities().await;
    Arc::clone(&daemon).run().await;
}
