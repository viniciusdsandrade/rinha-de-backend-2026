use serde::{Deserialize, Serialize};

use crate::{payload::Payload, refs::ReferenceSet, vector::vectorize};

#[derive(Debug, Clone, Copy, PartialEq, Deserialize, Serialize)]
pub struct Classification {
    pub approved: bool,
    pub fraud_score: f32,
}

#[derive(Debug, Clone)]
pub struct Classifier {
    refs: ReferenceSet,
}

impl Classifier {
    pub fn new(refs: ReferenceSet) -> Self {
        Self { refs }
    }

    pub fn classify(&self, payload: &Payload) -> Result<Classification, String> {
        if self.refs.len() < 5 {
            return Err("conjunto de referências insuficiente para top-5".to_string());
        }

        let query = vectorize(payload)?;
        let mut top = [(f32::INFINITY, false); 5];

        for index in 0..self.refs.len() {
            let distance = self.refs.distance_squared(&query, index);
            insert_top5(&mut top, distance, self.refs.labels()[index]);
        }

        let fraud_count = top.into_iter().filter(|(_, is_fraud)| *is_fraud).count() as f32;
        let fraud_score = fraud_count / 5.0;

        Ok(Classification {
            approved: fraud_score < 0.6,
            fraud_score,
        })
    }
}

fn insert_top5(top: &mut [(f32, bool); 5], distance: f32, is_fraud: bool) {
    if distance >= top[4].0 {
        return;
    }

    let mut position = 4;
    while position > 0 && top[position - 1].0 > distance {
        top[position] = top[position - 1];
        position -= 1;
    }
    top[position] = (distance, is_fraud);
}
