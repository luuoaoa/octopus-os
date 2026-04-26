// HAP mDNS advertisement via the system avahi-daemon, by shelling out to
// `avahi-publish-service`. avahi-daemon owns UDP/5353 (see the
// MulticastDNS=no drop-in for systemd-resolved); we publish _hap._tcp
// through D-Bus via the avahi-utils CLI rather than linking libavahi-client.
//
// Lifecycle: avahi-publish-service stays in the foreground for as long as
// it holds the registration. Killing it de-registers. To change TXT records
// (e.g. when configuration_number bumps) we kill the previous child and
// spawn a fresh one — same wire-level effect as a re-register either way.
// `kill_on_drop` ensures the child dies when MdnsResponder is dropped.

use std::process::Stdio;

use log::{debug, warn};
use tokio::process::{Child, Command};

use crate::pointer;

const PUBLISH_BIN: &str = "avahi-publish-service";
const SERVICE_TYPE: &str = "_hap._tcp";

pub struct MdnsResponder {
    config: pointer::Config,
    child: Option<Child>,
}

impl MdnsResponder {
    pub async fn new(config: pointer::Config) -> Self {
        MdnsResponder { config, child: None }
    }

    pub async fn update_records(&mut self) {
        debug!("attempting to set mDNS records");

        if let Some(mut prev) = self.child.take() {
            if let Err(e) = prev.start_kill() {
                warn!("failed to signal previous {}: {}", PUBLISH_BIN, e);
            }
            let _ = prev.wait().await;
        }

        let c = self.config.lock().await;
        let name = c.name.clone();
        let port = c.port;
        let tr = c.txt_records();
        drop(c);

        let mut cmd = Command::new(PUBLISH_BIN);
        cmd.arg(&name).arg(SERVICE_TYPE).arg(port.to_string());
        for r in tr.iter() {
            cmd.arg(r);
        }
        cmd.stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .kill_on_drop(true);

        debug!("setting mDNS records: {:?}", tr);

        match cmd.spawn() {
            Ok(child) => self.child = Some(child),
            Err(e) => warn!("failed to spawn {}: {}", PUBLISH_BIN, e),
        }
    }

    pub fn run_handle(&mut self) -> Box<dyn futures::Future<Output = ()> + Unpin + std::marker::Send> {
        // The actual mDNS work runs in avahi-daemon, driven by our spawned
        // avahi-publish-service child. There's nothing for us to drive on the
        // tokio runtime; park forever so try_join! waits on the HTTP server.
        Box::new(futures::future::pending::<()>())
    }
}
