use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct Payload {
    pub id: String,
    pub transaction: Transaction,
    pub customer: Customer,
    pub merchant: Merchant,
    pub terminal: Terminal,
    pub last_transaction: Option<LastTransaction>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct Transaction {
    pub amount: f32,
    pub installments: u32,
    pub requested_at: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct Customer {
    pub avg_amount: f32,
    pub tx_count_24h: u32,
    pub known_merchants: Vec<String>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct Merchant {
    pub id: String,
    pub mcc: String,
    pub avg_amount: f32,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct Terminal {
    pub is_online: bool,
    pub card_present: bool,
    pub km_from_home: f32,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct LastTransaction {
    pub timestamp: String,
    pub km_from_current: f32,
}
