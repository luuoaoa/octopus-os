use futures::{
    FutureExt,
    future::{BoxFuture, join_all},
};
use log::debug;
use serde_json::Value;
use std::{
    fmt::Debug,
    sync::{Arc, Mutex},
};
use uuid::Uuid;

#[derive(Debug)]
pub enum Event {
    ControllerPaired { id: Uuid },
    ControllerUnpaired { id: Uuid },
    CharacteristicValueChanged { aid: u64, iid: u64, value: Value },
}

pub type Listener = Box<dyn (for<'a> Fn(&'a Event) -> BoxFuture<'a, ()>) + Send + Sync>;

#[derive(Default)]
pub struct EventEmitter {
    listeners: slab::Slab<Listener>,
}

impl EventEmitter {
    pub fn new() -> EventEmitter {
        EventEmitter {
            listeners: slab::Slab::new(),
        }
    }

    pub fn add_listener(&mut self, listener: Listener) -> usize { self.listeners.insert(listener) }

    pub fn emit<'a>(&self, event: &'a Event) -> BoxFuture<'a, ()> {
        debug!("emitting event: {:?}", event);

        let futures: Vec<_> = self.listeners.iter().map(|(_, listener)| listener(event)).collect();

        async move {
            join_all(futures).await;
        }
        .boxed()
    }
}

pub struct EventListenerGuard {
    token: usize,
    emitter: Arc<Mutex<EventEmitter>>,
}

impl EventListenerGuard {
    pub fn new(token: usize, emitter: Arc<Mutex<EventEmitter>>) -> Self { EventListenerGuard { token, emitter } }
}

impl Drop for EventListenerGuard {
    fn drop(&mut self) {
        let mut emitter = self.emitter.lock().unwrap();
        drop(emitter.listeners.remove(self.token));
    }
}

macro_rules! emit {
    ($event_emitter:expr, $event:expr) => {{
        let event = $event;
        let fut = { $event_emitter.lock().unwrap().emit(event) };
        fut.await;
    }};
}
