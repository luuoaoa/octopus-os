use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::Arc;

use futures::FutureExt;
use serde::ser::{Serialize, SerializeStruct, Serializer};
use serde_json::{json, Value};
use tokio::sync::{mpsc, Mutex};
use tracing::{error, info, warn};

use hap::{
    accessory::{bridge::BridgeAccessory, AccessoryCategory, AccessoryInformation, HapAccessory},
    characteristic::AsyncCharacteristicCallbacks,
    server::{IpServer, Server},
    service::{
        accessory_information::AccessoryInformationService, fan::FanService, lightbulb::LightbulbService,
        HapService,
    },
    storage::{FileStorage, Storage},
    Config as HapConfig, HapType, MacAddress, Pin,
};

type AccessoryPtr = Arc<futures::lock::Mutex<Box<dyn HapAccessory>>>;

/// Commands emitted by HomeKit controllers that the daemon must execute.
/// Routed back through the same `handle_service` dispatcher used for HA events.
#[derive(Debug)]
pub struct HkCommand {
    pub domain: &'static str,   // "fan" | "light"
    pub service: &'static str,  // "turn_on" | "turn_off" | "set_percentage" | "set_direction"
    pub room: String,
    pub data: Value,
}

pub struct HomeKit {
    pub cmd_rx: Mutex<Option<mpsc::Receiver<HkCommand>>>,
    // Per-room accessory pointer (hap wraps each accessory in Arc<Mutex<Box<dyn HapAccessory>>>).
    accessories: HashMap<String, AccessoryPtr>,
    // Rooms whose on_update_async callbacks should be ignored because the change
    // originated from our own daemon (otherwise set_value loops back into the callback).
    suppress: Arc<Mutex<HashSet<String>>>,
}

impl HomeKit {
    /// Initialise the HomeKit bridge. Registers one CeilingFan accessory per room.
    /// Does NOT start the server — call [`run`] afterwards to drive it.
    pub async fn new(bridge_name: &str, rooms: &[String], port: u16, persist_dir: &str, pin: [u8; 8]) -> hap::Result<(Arc<Self>, IpServer)> {
        let (cmd_tx, cmd_rx) = mpsc::channel::<HkCommand>(32);
        let suppress: Arc<Mutex<HashSet<String>>> = Arc::new(Mutex::new(HashSet::new()));

        let bridge = BridgeAccessory::new(1, AccessoryInformation {
            name: bridge_name.into(),
            manufacturer: "onlyfansd".into(),
            model: "Bridge".into(),
            ..Default::default()
        })?;

        let mut accessories = HashMap::new();
        let mut ceiling_fans = Vec::new();
        for (i, room) in rooms.iter().enumerate() {
            let aid = (i as u64) + 2;
            let display = title_case(room) + " Fan";
            let mut acc = CeilingFanAccessory::new(aid, AccessoryInformation {
                name: display,
                manufacturer: "onlyfansd".into(),
                model: "Casa Vieja TR301A".into(),
                ..Default::default()
            })?;
            wire_callbacks(&mut acc, room.clone(), cmd_tx.clone(), Arc::clone(&suppress));
            ceiling_fans.push(acc);
        }

        // Persistent storage for HomeKit pairing data.
        std::fs::create_dir_all(persist_dir).ok();
        let mut storage = FileStorage::new(&PathBuf::from(persist_dir)).await?;

        let config = match storage.load_config().await {
            Ok(mut c) => {
                c.redetermine_local_ip();
                storage.save_config(&c).await?;
                c
            }
            Err(_) => {
                let c = HapConfig {
                    pin: Pin::new(pin)?,
                    name: bridge_name.into(),
                    device_id: random_mac(),
                    category: AccessoryCategory::Bridge,
                    port,
                    ..Default::default()
                };
                storage.save_config(&c).await?;
                c
            }
        };

        let server = IpServer::new(config, storage).await?;
        let bridge_ptr = server.add_accessory(bridge).await?;
        let _ = bridge_ptr; // we don't need to manipulate the bridge after registration

        for (room_name, acc) in rooms.iter().zip(ceiling_fans.into_iter()) {
            let ptr = server.add_accessory(acc).await?;
            accessories.insert(room_name.clone(), ptr);
        }

        Ok((
            Arc::new(Self {
                cmd_rx: Mutex::new(Some(cmd_rx)),
                accessories,
                suppress,
            }),
            server,
        ))
    }

    /// Push fan state from the daemon to the HomeKit characteristics.
    /// Suppresses the on_update_async callback for the duration — otherwise hap-rs's
    /// set_value fires the callback and we echo the command back into the daemon (loop).
    pub async fn update_fan(&self, room: &str, on: bool, percentage: u8, direction_forward: bool) {
        let Some(ptr) = self.accessories.get(room) else { return; };
        self.suppress.lock().await.insert(room.to_string());
        {
            let mut guard = ptr.lock().await;
            if let Some(svc) = guard.get_mut_service(HapType::Fan) {
                set_char(svc, HapType::PowerState, json!(on)).await;
                set_char(svc, HapType::RotationSpeed, json!(percentage as f32)).await;
                set_char(svc, HapType::RotationDirection, json!(if direction_forward { 0 } else { 1 })).await;
            }
        }
        self.suppress.lock().await.remove(room);
    }

    pub async fn update_light(&self, room: &str, on: bool) {
        let Some(ptr) = self.accessories.get(room) else { return; };
        self.suppress.lock().await.insert(room.to_string());
        {
            let mut guard = ptr.lock().await;
            if let Some(svc) = guard.get_mut_service(HapType::Lightbulb) {
                set_char(svc, HapType::PowerState, json!(on)).await;
            }
        }
        self.suppress.lock().await.remove(room);
    }
}

async fn set_char(svc: &mut dyn HapService, ht: HapType, val: Value) {
    if let Some(c) = svc.get_mut_characteristic(ht) {
        if let Err(e) = c.set_value(val).await {
            warn!("homekit set {:?}: {}", ht, e);
        }
    }
}

fn wire_callbacks(
    acc: &mut CeilingFanAccessory,
    room: String,
    tx: mpsc::Sender<HkCommand>,
    suppress: Arc<Mutex<HashSet<String>>>,
) {
    // Fan On
    {
        let tx = tx.clone();
        let room = room.clone();
        let suppress = Arc::clone(&suppress);
        acc.fan.power_state.on_update_async(Some(move |_cur: bool, new: bool| {
            let tx = tx.clone();
            let room = room.clone();
            let suppress = Arc::clone(&suppress);
            async move {
                if suppress.lock().await.contains(&room) { return Ok(()); }
                let cmd = if new {
                    HkCommand { domain: "fan", service: "turn_on", room, data: json!({"percentage": 35}) }
                } else {
                    HkCommand { domain: "fan", service: "turn_off", room, data: json!({}) }
                };
                let _ = tx.send(cmd).await;
                Ok(())
            }
            .boxed()
        }));
    }
    // Fan RotationSpeed (u8 percentage 0..100, delivered as f32 by hap for float chars)
    if let Some(c) = acc.fan.rotation_speed.as_mut() {
        let tx = tx.clone();
        let room = room.clone();
        let suppress = Arc::clone(&suppress);
        c.on_update_async(Some(move |_cur: f32, new: f32| {
            let tx = tx.clone();
            let room = room.clone();
            let suppress = Arc::clone(&suppress);
            async move {
                if suppress.lock().await.contains(&room) { return Ok(()); }
                let pct = new.round().clamp(0.0, 100.0) as u64;
                let cmd = if pct == 0 {
                    HkCommand { domain: "fan", service: "turn_off", room, data: json!({}) }
                } else {
                    HkCommand { domain: "fan", service: "set_percentage", room, data: json!({"percentage": pct}) }
                };
                let _ = tx.send(cmd).await;
                Ok(())
            }
            .boxed()
        }));
    }
    // Fan RotationDirection (0 = clockwise/forward, 1 = counter-clockwise/reverse)
    if let Some(c) = acc.fan.rotation_direction.as_mut() {
        let tx = tx.clone();
        let room = room.clone();
        let suppress = Arc::clone(&suppress);
        c.on_update_async(Some(move |_cur: i32, new: i32| {
            let tx = tx.clone();
            let room = room.clone();
            let suppress = Arc::clone(&suppress);
            async move {
                if suppress.lock().await.contains(&room) { return Ok(()); }
                let dir = if new == 1 { "reverse" } else { "forward" };
                let _ = tx.send(HkCommand {
                    domain: "fan", service: "set_direction", room,
                    data: json!({"direction": dir}),
                }).await;
                Ok(())
            }
            .boxed()
        }));
    }
    // Light On — Casa Vieja only supports toggle, so turn_on and turn_off both send "toggle"
    // and the daemon decides based on current state.
    {
        let tx = tx.clone();
        let room = room.clone();
        let suppress = Arc::clone(&suppress);
        acc.lightbulb.power_state.on_update_async(Some(move |_cur: bool, new: bool| {
            let tx = tx.clone();
            let room = room.clone();
            let suppress = Arc::clone(&suppress);
            async move {
                if suppress.lock().await.contains(&room) { return Ok(()); }
                let service = if new { "turn_on" } else { "turn_off" };
                let _ = tx.send(HkCommand {
                    domain: "light", service, room, data: json!({}),
                }).await;
                Ok(())
            }
            .boxed()
        }));
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

fn random_mac() -> MacAddress {
    use std::time::{SystemTime, UNIX_EPOCH};
    let n = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos() as u64;
    MacAddress::from([
        0x02, // locally administered
        ((n >> 32) & 0xff) as u8,
        ((n >> 24) & 0xff) as u8,
        ((n >> 16) & 0xff) as u8,
        ((n >> 8) & 0xff) as u8,
        (n & 0xff) as u8,
    ])
}

// ── Custom accessory: fan + lightbulb on a single accessory ──────────────────

#[derive(Debug)]
pub struct CeilingFanAccessory {
    id: u64,
    pub accessory_information: AccessoryInformationService,
    pub fan: FanService,
    pub lightbulb: LightbulbService,
}

impl CeilingFanAccessory {
    pub fn new(id: u64, info: AccessoryInformation) -> hap::Result<Self> {
        // Keep track of room by encoding it into the accessory info serial number slot
        // isn't needed — we store it on the struct directly.
        let accessory_information = info.to_service(1, id)?;
        let info_chars = accessory_information.get_characteristics().len() as u64;

        // Fan service starts after AccessoryInformation's characteristics + 1 (for the service IID itself).
        let fan_iid = 1 + info_chars + 1;
        let mut fan = FanService::new(fan_iid, id);
        fan.set_primary(true);
        // Drop the Name characteristic — iOS Home refuses accessories whose sub-service Name
        // is left at the default empty string.
        fan.name = None;

        // Lightbulb service IIDs follow after fan's. Fan has 4 chars (power_state + 3 optionals).
        let fan_chars_count: u64 = 4;
        let light_iid = fan_iid + fan_chars_count + 1;
        let mut lightbulb = LightbulbService::new(light_iid, id);
        // Drop adaptive-lighting/unused optionals (hap won't pair with them populated).
        lightbulb.brightness = None;
        lightbulb.color_temperature = None;
        lightbulb.hue = None;
        lightbulb.name = None;
        lightbulb.saturation = None;
        lightbulb.characteristic_value_active_transition_count = None;
        lightbulb.characteristic_value_transition_control = None;
        lightbulb.supported_characteristic_value_transition_configuration = None;

        Ok(Self {
            id,
            accessory_information,
            fan,
            lightbulb,
        })
    }
}

impl HapAccessory for CeilingFanAccessory {
    fn get_id(&self) -> u64 { self.id }
    fn set_id(&mut self, id: u64) { self.id = id; }

    fn get_service(&self, hap_type: HapType) -> Option<&dyn HapService> {
        self.get_services().into_iter().find(|s| s.get_type() == hap_type)
    }

    fn get_mut_service(&mut self, hap_type: HapType) -> Option<&mut dyn HapService> {
        for s in self.get_mut_services() {
            if s.get_type() == hap_type { return Some(s); }
        }
        None
    }

    fn get_services(&self) -> Vec<&dyn HapService> {
        vec![&self.accessory_information, &self.fan, &self.lightbulb]
    }

    fn get_mut_services(&mut self) -> Vec<&mut dyn HapService> {
        vec![&mut self.accessory_information, &mut self.fan, &mut self.lightbulb]
    }
}

impl Serialize for CeilingFanAccessory {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        let mut state = serializer.serialize_struct("HapAccessory", 2)?;
        state.serialize_field("aid", &self.get_id())?;
        state.serialize_field("services", &self.get_services())?;
        state.end()
    }
}

/// Drive the server. Returns when the server exits.
pub async fn run_server(server: IpServer) {
    let handle = server.run_handle();
    if let Err(e) = handle.await {
        error!("HomeKit server exited: {}", e);
    }
    info!("HomeKit server stopped");
}
