#include "rinha/refs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <zlib.h>

#include "simdjson.h"

namespace rinha {

namespace {

constexpr std::size_t kFloatSizeBytes = sizeof(float);

std::uint32_t group_key(const ReferenceSet& refs, std::size_t row) {
    const bool no_last_transaction = refs.dim(5)[row] < -0.5f && refs.dim(6)[row] < -0.5f;
    const auto bool_bit = [&refs, row](std::size_t dim) -> std::uint32_t {
        return refs.dim(dim)[row] > 0.5f ? 1U : 0U;
    };
    const auto risk_bucket = static_cast<std::uint32_t>(std::lround(refs.dim(12)[row] * 20.0f));

    return (no_last_transaction ? 1U : 0U) |
           (bool_bit(9) << 1U) |
           (bool_bit(10) << 2U) |
           (bool_bit(11) << 3U) |
           (risk_bucket << 4U);
}

bool read_binary_file(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "falha ao abrir arquivo binário: " + path;
        return false;
    }
    file.seekg(0, std::ios::end);
    const auto length = file.tellg();
    file.seekg(0, std::ios::beg);
    if (length < 0) {
        error = "falha ao medir arquivo binário: " + path;
        return false;
    }
    bytes.resize(static_cast<std::size_t>(length));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            error = "falha ao ler arquivo binário: " + path;
            return false;
        }
    }
    return true;
}

bool decompress_gzip_file(const std::string& path, std::string& output, std::string& error) {
    gzFile file = gzopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "falha ao abrir gzip: " + path;
        return false;
    }

    output.clear();
    std::array<char, 1 << 15> buffer{};
    int bytes_read = 0;
    while ((bytes_read = gzread(file, buffer.data(), static_cast<unsigned int>(buffer.size()))) > 0) {
        output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
    }

    if (bytes_read < 0) {
        int zlib_error = Z_OK;
        const char* message = gzerror(file, &zlib_error);
        error = "falha ao descompactar gzip: ";
        error += message == nullptr ? "erro desconhecido" : message;
        gzclose(file);
        return false;
    }

    gzclose(file);
    return true;
}

}  // namespace

bool ReferenceSet::load_gzip_json(const std::string& path, ReferenceSet& refs, std::string& error) {
    std::string decompressed;
    if (!decompress_gzip_file(path, decompressed, error)) {
        return false;
    }

    thread_local simdjson::dom::parser parser;
    simdjson::padded_string json(decompressed);
    simdjson::dom::element root;
    if (const auto code = parser.parse(json).get(root); code != simdjson::SUCCESS) {
        error = "falha ao decodificar referências";
        return false;
    }

    simdjson::dom::array entries;
    if (const auto code = root.get(entries); code != simdjson::SUCCESS) {
        error = "formato inválido de referências";
        return false;
    }

    refs = ReferenceSet{};
    std::size_t count = 0;
    for (const auto entry : entries) {
        simdjson::dom::object object;
        if (const auto code = entry.get(object); code != simdjson::SUCCESS) {
            error = "entrada inválida em referências";
            return false;
        }

        QueryVector vector{};
        std::size_t dim_index = 0;
        std::uint8_t label = 0;
        bool seen_vector = false;
        bool seen_label = false;

        for (const auto field : object) {
            const std::string_view key = field.key;
            if (key == "vector") {
                simdjson::dom::array values;
                if (const auto code = field.value.get(values); code != simdjson::SUCCESS) {
                    error = "vetor de referência inválido";
                    return false;
                }

                dim_index = 0;
                for (const auto value : values) {
                    if (dim_index >= kDimensions) {
                        error = "vetor de referência não possui 14 dimensões";
                        return false;
                    }
                    double number = 0.0;
                    if (const auto code = value.get(number); code != simdjson::SUCCESS) {
                        error = "vetor de referência inválido";
                        return false;
                    }
                    vector[dim_index++] = static_cast<float>(number);
                }
                if (dim_index != kDimensions) {
                    error = "vetor de referência não possui 14 dimensões";
                    return false;
                }
                seen_vector = true;
            } else if (key == "label") {
                std::string_view label_value;
                if (const auto code = field.value.get(label_value); code != simdjson::SUCCESS) {
                    error = "label inválida";
                    return false;
                }
                if (label_value == "fraud") {
                    label = 1;
                } else if (label_value == "legit") {
                    label = 0;
                } else {
                    error = "label de referência inválida: " + std::string(label_value);
                    return false;
                }
                seen_label = true;
            }
        }

        if (!seen_vector || !seen_label) {
            error = "entrada de referência incompleta";
            return false;
        }

        for (std::size_t index = 0; index < kDimensions; ++index) {
            refs.dims_[index].push_back(vector[index]);
        }
        refs.labels_.push_back(label);
        ++count;
    }

    if (count == 0) {
        error = "conjunto de referências vazio";
        return false;
    }

    refs.finalize_dimension_order();
    return true;
}

bool ReferenceSet::load_binary(
    const std::string& references_path,
    const std::string& labels_path,
    ReferenceSet& refs,
    std::string& error
) {
    std::vector<std::uint8_t> ref_bytes;
    std::vector<std::uint8_t> label_bytes;
    if (!read_binary_file(references_path, ref_bytes, error) || !read_binary_file(labels_path, label_bytes, error)) {
        return false;
    }

    if (label_bytes.empty()) {
        error = "labels.bin está vazio";
        return false;
    }

    const std::size_t row_count = label_bytes.size();
    const std::size_t expected_bytes = row_count * kDimensions * kFloatSizeBytes;
    if (ref_bytes.size() != expected_bytes) {
        error = "references.bin possui tamanho inválido";
        return false;
    }

    refs = ReferenceSet{};
    refs.labels_ = std::move(label_bytes);
    for (auto& dim : refs.dims_) {
        dim.reserve(row_count);
    }

    const std::size_t chunk_len = row_count * kFloatSizeBytes;
    for (std::size_t dim_index = 0; dim_index < kDimensions; ++dim_index) {
        const std::size_t offset = dim_index * chunk_len;
        const std::uint8_t* chunk = ref_bytes.data() + offset;
        for (std::size_t row = 0; row < row_count; ++row) {
            float value = 0.0f;
            std::memcpy(&value, chunk + (row * kFloatSizeBytes), kFloatSizeBytes);
            refs.dims_[dim_index].push_back(value);
        }
    }

    refs.finalize_dimension_order();
    return true;
}

bool ReferenceSet::write_binary(
    const std::string& references_path,
    const std::string& labels_path,
    std::string& error
) const {
    std::ofstream refs_file(references_path, std::ios::binary);
    if (!refs_file) {
        error = "falha ao criar references.bin: " + references_path;
        return false;
    }

    std::ofstream labels_file(labels_path, std::ios::binary);
    if (!labels_file) {
        error = "falha ao criar labels.bin: " + labels_path;
        return false;
    }

    for (const auto& dim : dims_) {
        for (const float value : dim) {
            refs_file.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(value)));
            if (!refs_file) {
                error = "falha ao escrever references.bin: " + references_path;
                return false;
            }
        }
    }

    if (!labels_.empty()) {
        labels_file.write(
            reinterpret_cast<const char*>(labels_.data()),
            static_cast<std::streamsize>(labels_.size())
        );
        if (!labels_file) {
            error = "falha ao escrever labels.bin: " + labels_path;
            return false;
        }
    }

    return true;
}

std::size_t ReferenceSet::len() const noexcept {
    return labels_.size();
}

bool ReferenceSet::is_empty() const noexcept {
    return labels_.empty();
}

bool ReferenceSet::is_fraud(std::size_t index) const noexcept {
    return labels_[index] != 0;
}

const std::vector<float>& ReferenceSet::dim(std::size_t index) const noexcept {
    return dims_[index];
}

const std::vector<std::uint8_t>& ReferenceSet::labels() const noexcept {
    return labels_;
}

const std::vector<ReferenceGroup>& ReferenceSet::groups() const noexcept {
    return groups_;
}

const std::array<std::size_t, kDimensions>& ReferenceSet::dimension_order() const noexcept {
    return dimension_order_;
}

bool ReferenceSet::distance_squared_if_below(
    const QueryVector& query,
    std::size_t row,
    float limit,
    float& distance
) const noexcept {
    float sum = 0.0f;
    for (const std::size_t dim_index : dimension_order_) {
        const float delta = query[dim_index] - dims_[dim_index][row];
        sum += delta * delta;
        if (sum >= limit) {
            return false;
        }
    }
    distance = sum;
    return true;
}

void ReferenceSet::finalize_dimension_order() noexcept {
    groups_.clear();

    for (auto& dim : dims_) {
        if (dim.capacity() > dim.size()) {
            dim.shrink_to_fit();
        }
    }

    for (std::size_t index = 0; index < kDimensions; ++index) {
        dimension_order_[index] = index;
    }

    if (labels_.empty()) {
        return;
    }

    std::array<float, kDimensions> variance{};
    const float row_count = static_cast<float>(labels_.size());

    for (std::size_t dim_index = 0; dim_index < kDimensions; ++dim_index) {
        const auto& dim = dims_[dim_index];
        double sum = 0.0;
        double squared_sum = 0.0;
        for (const float value : dim) {
            sum += value;
            squared_sum += static_cast<double>(value) * static_cast<double>(value);
        }
        const double mean = sum / row_count;
        variance[dim_index] = static_cast<float>((squared_sum / row_count) - (mean * mean));
    }

    std::sort(
        dimension_order_.begin(),
        dimension_order_.end(),
        [&variance](std::size_t left, std::size_t right) {
            return variance[left] > variance[right];
        }
    );

    build_groups();
}

void ReferenceSet::build_groups() {
    std::unordered_map<std::uint32_t, std::size_t> group_by_key;
    groups_.clear();
    groups_.reserve(128);

    for (std::size_t row = 0; row < len(); ++row) {
        const std::uint32_t key = group_key(*this, row);
        const auto [position, inserted] = group_by_key.emplace(key, groups_.size());
        if (inserted) {
            groups_.emplace_back();
        }

        ReferenceGroup& group = groups_[position->second];
        for (std::size_t dim_index = 0; dim_index < kDimensions; ++dim_index) {
            group.dims[dim_index].push_back(dims_[dim_index][row]);
        }
        group.labels.push_back(labels_[row]);
    }

    for (ReferenceGroup& group : groups_) {
        for (std::size_t dim_index = 0; dim_index < kDimensions; ++dim_index) {
            const auto [min_it, max_it] = std::minmax_element(
                group.dims[dim_index].begin(),
                group.dims[dim_index].end()
            );
            group.min_values[dim_index] = *min_it;
            group.max_values[dim_index] = *max_it;
            if (group.dims[dim_index].capacity() > group.dims[dim_index].size()) {
                group.dims[dim_index].shrink_to_fit();
            }
        }
        if (group.labels.capacity() > group.labels.size()) {
            group.labels.shrink_to_fit();
        }
    }
    if (groups_.capacity() > groups_.size()) {
        groups_.shrink_to_fit();
    }
}

}  // namespace rinha
