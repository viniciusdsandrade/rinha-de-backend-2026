use serde::{Deserialize, Serialize};

use crate::{payload::Payload, refs::ReferenceSet, vector::vectorize};

#[derive(Debug, Clone, Copy, PartialEq, Deserialize, Serialize)]
pub struct Classification {
    pub approved: bool,
    pub fraud_score: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SearchMode {
    Auto,
    Scalar,
    Avx2,
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
        self.classify_with_mode(payload, SearchMode::Auto)
    }

    pub fn classify_with_mode(
        &self,
        payload: &Payload,
        search_mode: SearchMode,
    ) -> Result<Classification, String> {
        if self.refs.len() < 5 {
            return Err("conjunto de referências insuficiente para top-5".to_string());
        }

        let query = vectorize(payload)?;
        let top = match search_mode {
            SearchMode::Auto => {
                if Self::supports_avx2() {
                    // SAFETY: gated by runtime feature detection.
                    unsafe { self.top5_avx2(&query) }
                } else {
                    self.top5_scalar(&query)
                }
            }
            SearchMode::Scalar => self.top5_scalar(&query),
            SearchMode::Avx2 => {
                if !Self::supports_avx2() {
                    return Err("AVX2/FMA não disponível neste host".to_string());
                }
                // SAFETY: gated by runtime feature detection.
                unsafe { self.top5_avx2(&query) }
            }
        };

        let fraud_count = top.into_iter().filter(|(_, is_fraud)| *is_fraud).count() as f32;
        let fraud_score = fraud_count / 5.0;

        Ok(Classification {
            approved: fraud_score < 0.6,
            fraud_score,
        })
    }

    pub fn supports_avx2() -> bool {
        #[cfg(target_arch = "x86_64")]
        {
            std::arch::is_x86_feature_detected!("avx2")
                && std::arch::is_x86_feature_detected!("fma")
        }

        #[cfg(not(target_arch = "x86_64"))]
        {
            false
        }
    }

    fn top5_scalar(&self, query: &[f32; 14]) -> [(f32, bool); 5] {
        let mut top = [(f32::INFINITY, false); 5];

        for index in 0..self.refs.len() {
            if let Some(distance) = self
                .refs
                .distance_squared_if_below(query, index, top[4].0)
            {
                insert_top5(&mut top, distance, self.refs.is_fraud(index));
            }
        }

        top
    }

    #[cfg(target_arch = "x86_64")]
    #[target_feature(enable = "avx2,fma")]
    unsafe fn top5_avx2(&self, query: &[f32; 14]) -> [(f32, bool); 5] {
        use std::arch::x86_64::{
            __m256, _CMP_GE_OQ, _mm256_cmp_ps, _mm256_fmadd_ps, _mm256_loadu_ps,
            _mm256_movemask_ps, _mm256_set1_ps, _mm256_setzero_ps, _mm256_storeu_ps,
            _mm256_sub_ps,
        };

        let mut top = [(f32::INFINITY, false); 5];
        let rows = self.refs.len();
        let chunks = rows / 8;
        let labels = self.refs.labels();
        let dims: [&[f32]; 14] = core::array::from_fn(|index| self.refs.dim(index));
        let ordered_dims = self.refs.dimension_order();
        let query_lanes: [__m256; 14] = core::array::from_fn(|index| _mm256_set1_ps(query[index]));

        for chunk in 0..chunks {
            let mut dot: __m256 = _mm256_setzero_ps();
            let offset = chunk * 8;
            let threshold = top[4].0;
            let threshold_lanes = _mm256_set1_ps(threshold);
            let mut pruned = false;

            for &dim in ordered_dims {
                let refs_lane = unsafe { _mm256_loadu_ps(dims[dim].as_ptr().add(offset)) };
                let diff = _mm256_sub_ps(query_lanes[dim], refs_lane);
                dot = _mm256_fmadd_ps(diff, diff, dot);

                if threshold.is_finite() {
                    let cmp = _mm256_cmp_ps(dot, threshold_lanes, _CMP_GE_OQ);
                    if _mm256_movemask_ps(cmp) == 0xFF {
                        pruned = true;
                        break;
                    }
                }
            }

            if pruned {
                continue;
            }

            let mut distances = [0.0f32; 8];
            unsafe { _mm256_storeu_ps(distances.as_mut_ptr(), dot) };

            for lane in 0..8 {
                insert_top5(&mut top, distances[lane], labels[offset + lane] != 0);
            }
        }

        for index in (chunks * 8)..rows {
            if let Some(distance) = self
                .refs
                .distance_squared_if_below(query, index, top[4].0)
            {
                insert_top5(&mut top, distance, labels[index] != 0);
            }
        }

        top
    }

    #[cfg(not(target_arch = "x86_64"))]
    unsafe fn top5_avx2(&self, _query: &[f32; 14]) -> [(f32, bool); 5] {
        unreachable!("AVX2 só está implementado em x86_64");
    }
}

#[inline(always)]
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
