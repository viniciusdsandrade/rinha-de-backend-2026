use std::{
    array,
    fs::{self, File},
    io::{BufReader, Read, Write},
    path::Path,
};

use flate2::bufread::GzDecoder;
use serde::Deserialize;

const DIMENSIONS: usize = 14;
const FLOAT_SIZE_BYTES: usize = std::mem::size_of::<f32>();

#[derive(Debug, Clone)]
pub struct ReferenceSet {
    dims: [Vec<f32>; DIMENSIONS],
    labels: Vec<u8>,
}

impl ReferenceSet {
    pub fn load_json(path: impl AsRef<Path>) -> Result<Self, String> {
        let file = File::open(path.as_ref())
            .map_err(|error| format!("falha ao abrir referências: {error}"))?;
        Self::load_json_reader(BufReader::new(file))
    }

    pub fn load_gzip_json(path: impl AsRef<Path>) -> Result<Self, String> {
        let file = File::open(path.as_ref())
            .map_err(|error| format!("falha ao abrir referências: {error}"))?;
        let reader = BufReader::new(file);
        let decoder = GzDecoder::new(reader);
        Self::load_json_reader(decoder)
    }

    pub fn load_binary(
        references_path: impl AsRef<Path>,
        labels_path: impl AsRef<Path>,
    ) -> Result<Self, String> {
        let refs_bytes = fs::read(references_path.as_ref())
            .map_err(|error| format!("falha ao ler references.bin: {error}"))?;
        let label_bytes = fs::read(labels_path.as_ref())
            .map_err(|error| format!("falha ao ler labels.bin: {error}"))?;

        if label_bytes.is_empty() {
            return Err("labels.bin está vazio".to_string());
        }

        let row_count = label_bytes.len();
        let expected_bytes = row_count * DIMENSIONS * FLOAT_SIZE_BYTES;

        if refs_bytes.len() != expected_bytes {
            return Err(format!(
                "references.bin possui {} bytes, esperado {} para {row_count} referências",
                refs_bytes.len(),
                expected_bytes
            ));
        }

        let mut dims = array::from_fn(|_| Vec::with_capacity(row_count));
        let chunk_len = row_count * FLOAT_SIZE_BYTES;

        for (dim_index, dim) in dims.iter_mut().enumerate() {
            let offset = dim_index * chunk_len;
            let chunk = &refs_bytes[offset..offset + chunk_len];

            for bytes in chunk.chunks_exact(FLOAT_SIZE_BYTES) {
                dim.push(f32::from_le_bytes(bytes.try_into().unwrap()));
            }
        }

        let labels = label_bytes;

        Ok(Self { dims, labels })
    }

    pub fn write_binary(
        &self,
        references_path: impl AsRef<Path>,
        labels_path: impl AsRef<Path>,
    ) -> Result<(), String> {
        let mut refs_file = File::create(references_path.as_ref())
            .map_err(|error| format!("falha ao criar references.bin: {error}"))?;
        let mut labels_file = File::create(labels_path.as_ref())
            .map_err(|error| format!("falha ao criar labels.bin: {error}"))?;

        for dim in &self.dims {
            for value in dim {
                refs_file
                    .write_all(&value.to_le_bytes())
                    .map_err(|error| format!("falha ao escrever references.bin: {error}"))?;
            }
        }

        for label in &self.labels {
            labels_file
                .write_all(&[*label])
                .map_err(|error| format!("falha ao escrever labels.bin: {error}"))?;
        }

        Ok(())
    }

    pub fn len(&self) -> usize {
        self.labels.len()
    }

    pub fn is_empty(&self) -> bool {
        self.labels.is_empty()
    }

    pub fn labels(&self) -> &[u8] {
        &self.labels
    }

    #[inline(always)]
    pub fn is_fraud(&self, index: usize) -> bool {
        self.labels[index] != 0
    }

    pub fn dim(&self, index: usize) -> &[f32] {
        &self.dims[index]
    }

    pub fn distance_squared(&self, query: &[f32; DIMENSIONS], row: usize) -> f32 {
        let mut sum = 0.0;

        for (dim_index, query_value) in query.iter().enumerate() {
            let delta = query_value - self.dims[dim_index][row];
            sum += delta * delta;
        }

        sum
    }

    fn load_json_reader(reader: impl Read) -> Result<Self, String> {
        let entries: Vec<ReferenceEntry> = serde_json::from_reader(reader)
            .map_err(|error| format!("falha ao decodificar referências: {error}"))?;

        let mut dims = array::from_fn(|_| Vec::with_capacity(entries.len()));
        let mut labels = Vec::with_capacity(entries.len());

        for entry in entries {
            if entry.vector.len() != DIMENSIONS {
                return Err("vetor de referência não possui 14 dimensões".to_string());
            }

            for (dim_index, value) in entry.vector.into_iter().enumerate() {
                dims[dim_index].push(value);
            }
            let label = match entry.label.as_str() {
                "fraud" => 1,
                "legit" => 0,
                other => {
                    return Err(format!("label de referência inválida: {other}"));
                }
            };
            labels.push(label);
        }

        Ok(Self { dims, labels })
    }
}

#[derive(Debug, Deserialize)]
struct ReferenceEntry {
    vector: Vec<f32>,
    label: String,
}

#[cfg(test)]
mod tests {
    use super::ReferenceSet;

    #[test]
    fn rejects_unknown_labels() {
        let json = r#"[{"vector":[0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0],"label":"fruad"}]"#;

        let error = ReferenceSet::load_json_reader(json.as_bytes()).unwrap_err();

        assert!(error.contains("label de referência inválida: fruad"));
    }
}
