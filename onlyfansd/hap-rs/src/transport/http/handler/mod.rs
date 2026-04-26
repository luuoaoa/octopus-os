use hyper::{self, Body, Response, StatusCode, Uri};

use crate::{
    Error,
    Result as HapResult,
    pointer,
    tlv::{self, Encodable},
    transport::http::{status_response, tlv_response},
};

pub mod accessories;
pub mod characteristics;
pub mod identify;
pub mod pair_setup;
pub mod pair_verify;
pub mod pairings;

pub trait HandlerExt {
    async fn handle(
        &mut self,
        uri: Uri,
        body: Body,
        controller_id: pointer::ControllerId,
        event_subscriptions: pointer::EventSubscriptions,
        config: pointer::Config,
        storage: pointer::Storage,
        accessory_database: pointer::AccessoryDatabase,
        event_emitter: pointer::EventEmitter,
    ) -> HapResult<Response<Body>>;
}

pub trait TlvHandlerExt {
    type ParseResult: Send;
    type Result: Encodable;

    async fn parse(&self, body: Body) -> Result<Self::ParseResult, tlv::ErrorContainer>;
    async fn handle(
        &mut self,
        step: Self::ParseResult,
        controller_id: pointer::ControllerId,
        config: pointer::Config,
        storage: pointer::Storage,
        event_emitter: pointer::EventEmitter,
    ) -> Result<Self::Result, tlv::ErrorContainer>;
}

#[derive(Debug)]
pub struct TlvHandler<T: TlvHandlerExt + Send + Sync>(T);

impl<T: TlvHandlerExt + Send + Sync> From<T> for TlvHandler<T> {
    fn from(inst: T) -> TlvHandler<T> { TlvHandler(inst) }
}

impl<T: TlvHandlerExt + Send + Sync> HandlerExt for TlvHandler<T> {
    async fn handle(
        &mut self,
        _: Uri,
        body: Body,
        controller_id: pointer::ControllerId,
        _: pointer::EventSubscriptions,
        config: pointer::Config,
        storage: pointer::Storage,
        _: pointer::AccessoryDatabase,
        event_emitter: pointer::EventEmitter,
    ) -> HapResult<Response<Body>> {
        let response = match self.0.parse(body).await {
            Err(e) => e.encode(),
            Ok(step) => match self.0.handle(step, controller_id, config, storage, event_emitter).await {
                Err(e) => e.encode(),
                Ok(res) => res.encode(),
            },
        };
        tlv_response(response, StatusCode::OK)
    }
}

pub trait JsonHandlerExt {
    async fn handle(
        &mut self,
        uri: Uri,
        body: Body,
        controller_id: pointer::ControllerId,
        event_subscriptions: pointer::EventSubscriptions,
        config: pointer::Config,
        storage: pointer::Storage,
        accessory_database: pointer::AccessoryDatabase,
        event_emitter: pointer::EventEmitter,
    ) -> HapResult<Response<Body>>;
}

#[derive(Debug)]
pub struct JsonHandler<T: JsonHandlerExt + Send + Sync>(T);

impl<T: JsonHandlerExt + Send + Sync> From<T> for JsonHandler<T> {
    fn from(inst: T) -> JsonHandler<T> { JsonHandler(inst) }
}

impl<T: JsonHandlerExt + Send + Sync> HandlerExt for JsonHandler<T> {
    async fn handle(
        &mut self,
        uri: Uri,
        body: Body,
        controller_id: pointer::ControllerId,
        event_subscriptions: pointer::EventSubscriptions,
        config: pointer::Config,
        storage: pointer::Storage,
        accessory_database: pointer::AccessoryDatabase,
        event_emitter: pointer::EventEmitter,
    ) -> HapResult<Response<Body>> {
        match self
            .0
            .handle(
                uri,
                body,
                controller_id,
                event_subscriptions,
                config,
                storage,
                accessory_database,
                event_emitter,
            )
            .await
        {
            Ok(res) => Ok(res),
            Err(e) => match e {
                Error::HttpStatus(status) => status_response(status),
                _ => status_response(StatusCode::INTERNAL_SERVER_ERROR),
            },
        }
    }
}
