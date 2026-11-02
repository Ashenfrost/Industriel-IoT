//! Modtager beskeder fra MQTT-brokeren og sender dem videre som `RawMessage`.
//! Dette modul ved INTET om Sparkplug B eller QuestDB.

use std::sync::mpsc::Sender;
use std::time::Duration;

use anyhow::Result;
use rumqttc::{Client, Event, MqttOptions, Packet, QoS};

use crate::model::RawMessage;

pub struct MqttConfig {
    pub host: String,
    pub port: u16,
    pub client_id: String,
    pub topic: String,
    pub username: Option<String>,
    pub password: Option<String>,
}

/// Forbinder, abonnerer og sender hver modtaget besked ind i `tx`.
/// Funktionen kører "for evigt" - den skal derfor køres i sin egen tråd.
pub fn run(cfg: MqttConfig, tx: Sender<RawMessage>) -> Result<()> {
    let mut opts = MqttOptions::new(&cfg.client_id, &cfg.host, cfg.port);
    opts.set_keep_alive(Duration::from_secs(30));
    if let (Some(user), Some(pass)) = (&cfg.username, &cfg.password) {
        opts.set_credentials(user, pass);
    }

    let (client, mut connection) = Client::new(opts, 10);
    client.subscribe(&cfg.topic, QoS::AtLeastOnce)?;
    println!("[mqtt] abonnerer på '{}' på {}:{}", cfg.topic, cfg.host, cfg.port);

    // `connection.iter()` driver netværket og giver os events efterhånden.
    for event in connection.iter() {
        match event {
            Ok(Event::Incoming(Packet::Publish(p))) => {
                let msg = RawMessage {
                    topic: p.topic,
                    payload: p.payload.to_vec(),
                };
                // Fejler send, er modtageren (main) lukket ned - så stopper vi.
                if tx.send(msg).is_err() {
                    break;
                }
            }
            Ok(_) => {} // pings, acks osv. interesserer os ikke
            Err(e) => {
                eprintln!("[mqtt] fejl: {e} - prøver igen om 2 sek.");
                std::thread::sleep(Duration::from_secs(2));
            }
        }
    }
    Ok(())
}
