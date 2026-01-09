#include "memglass/observer.hpp"

#include <cstring>

namespace memglass {

// FieldProxy implementation

FieldProxy::FieldProxy(ObjectView& obj, const FieldEntry* field, void* data)
    : obj_(obj)
    , field_(field)
    , data_(data)
{
}

FieldProxy FieldProxy::operator[](std::string_view name) const {
    if (!field_ || !data_) {
        return FieldProxy(obj_, nullptr, nullptr);
    }

    // Get the nested type
    const ObservedType* nested_type = obj_.observer_->get_type(field_->type_id);
    if (!nested_type) {
        return FieldProxy(obj_, nullptr, nullptr);
    }

    // Find the field in the nested type
    for (const auto& f : nested_type->fields) {
        if (std::string_view(f.name) == name) {
            void* field_data = static_cast<char*>(data_) + f.offset;
            return FieldProxy(obj_, &f, field_data);
        }
    }

    return FieldProxy(obj_, nullptr, nullptr);
}

FieldProxy FieldProxy::operator[](size_t index) const {
    if (!field_ || !data_) {
        return FieldProxy(obj_, nullptr, nullptr);
    }

    if (!(field_->flags & static_cast<uint32_t>(FieldFlags::IsArray))) {
        return FieldProxy(obj_, nullptr, nullptr);
    }

    if (index >= field_->array_size) {
        return FieldProxy(obj_, nullptr, nullptr);
    }

    // Calculate element size (field size / array_size)
    size_t element_size = field_->size / field_->array_size;
    void* element_data = static_cast<char*>(data_) + index * element_size;

    // Create a temporary field entry for the element
    // Note: This is a simplification - in a full implementation we'd need
    // to handle this more elegantly
    return FieldProxy(obj_, field_, element_data);
}

// ObjectView implementation

ObjectView::ObjectView(Observer& observer, const ObservedObject& obj_info)
    : observer_(&observer)
    , obj_info_(obj_info)
{
    type_ = observer.get_type(obj_info.type_id);
    data_ = observer.get_object_data(obj_info.region_id, obj_info.offset);
}

FieldProxy ObjectView::operator[](std::string_view field_name) {
    // First try exact match (handles flat dotted names like "position.symbol_id")
    const FieldEntry* field = find_field(field_name);
    if (field && data_) {
        void* field_data = static_cast<char*>(data_) + field->offset;
        return FieldProxy(*this, field, field_data);
    }

    // Fallback: Handle dot notation for nested types
    size_t dot_pos = field_name.find('.');
    if (dot_pos != std::string_view::npos) {
        std::string_view first = field_name.substr(0, dot_pos);
        std::string_view rest = field_name.substr(dot_pos + 1);
        return (*this)[first][rest];
    }

    return FieldProxy(*this, nullptr, nullptr);
}

const FieldEntry* ObjectView::find_field(std::string_view name) const {
    if (!type_) return nullptr;

    for (const auto& field : type_->fields) {
        if (std::string_view(field.name) == name) {
            return &field;
        }
    }
    return nullptr;
}

// Observer implementation

Observer::Observer(std::string_view session_name)
    : session_name_(session_name)
{
}

Observer::~Observer() {
    disconnect();
}

bool Observer::connect() {
    if (connected_) return true;

    // Open header shared memory
    std::string header_shm_name = detail::make_header_shm_name(session_name_);
    if (!header_shm_.open(header_shm_name)) {
        return false;
    }

    header_ = static_cast<TelemetryHeader*>(header_shm_.data());

    // Verify magic
    if (header_->magic != HEADER_MAGIC) {
        header_shm_.close();
        header_ = nullptr;
        return false;
    }

    // Verify version
    if (header_->version != PROTOCOL_VERSION) {
        header_shm_.close();
        header_ = nullptr;
        return false;
    }

    connected_ = true;

    // Load initial state
    refresh();

    return true;
}

void Observer::disconnect() {
    if (!connected_) return;

    region_shms_.clear();
    overflow_shms_.clear();
    types_.clear();
    type_id_to_index_.clear();
    header_shm_.close();
    header_ = nullptr;
    connected_ = false;
}

void Observer::refresh() {
    if (!connected_) return;

    uint64_t current_seq = header_->sequence.load(std::memory_order_acquire);
    if (current_seq != last_sequence_) {
        load_types();
        load_regions();
        load_overflow_regions();
        last_sequence_ = current_seq;
    }
}

uint64_t Observer::producer_pid() const {
    if (!header_) return 0;
    return header_->producer_pid;
}

uint64_t Observer::start_timestamp() const {
    if (!header_) return 0;
    return header_->start_timestamp;
}

uint64_t Observer::sequence() const {
    if (!header_) return 0;
    return header_->sequence.load(std::memory_order_acquire);
}

std::vector<ObservedObject> Observer::objects() const {
    std::vector<ObservedObject> result;
    if (!header_) return result;

    // Helper lambda to add object entries from a memory region
    auto add_objects = [&](const ObjectEntry* entries, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            ObjectState state = static_cast<ObjectState>(
                entries[i].state.load(std::memory_order_acquire));
            if (state != ObjectState::Alive) continue;

            ObservedObject obj;
            obj.label = entries[i].label;
            obj.type_id = entries[i].type_id;
            obj.region_id = entries[i].region_id;
            obj.offset = entries[i].offset;
            obj.generation = entries[i].generation;
            obj.state = state;

            // Get type name
            auto it = type_id_to_index_.find(obj.type_id);
            if (it != type_id_to_index_.end()) {
                obj.type_name = types_[it->second].name;
            }

            result.push_back(std::move(obj));
        }
    };

    // Add objects from header
    uint32_t count = header_->object_count.load(std::memory_order_acquire);
    auto* entries = reinterpret_cast<const ObjectEntry*>(
        static_cast<const char*>(header_shm_.data()) + header_->object_dir_offset);
    add_objects(entries, count);

    // Add objects from overflow regions
    for (const auto& [id, shm] : overflow_shms_) {
        auto* desc = static_cast<const MetadataOverflowDescriptor*>(shm.data());
        uint32_t overflow_count = desc->object_entry_count.load(std::memory_order_acquire);
        auto* overflow_entries = reinterpret_cast<const ObjectEntry*>(
            static_cast<const char*>(shm.data()) + desc->object_entry_offset);
        add_objects(overflow_entries, overflow_count);
    }

    return result;
}

ObjectView Observer::find(std::string_view label) {
    if (!header_) return ObjectView();

    // Helper lambda to search for an object by label
    auto search_entries = [&](const ObjectEntry* entries, uint32_t count) -> std::optional<ObservedObject> {
        for (uint32_t i = 0; i < count; ++i) {
            ObjectState state = static_cast<ObjectState>(
                entries[i].state.load(std::memory_order_acquire));
            if (state != ObjectState::Alive) continue;

            if (std::string_view(entries[i].label) == label) {
                ObservedObject obj;
                obj.label = entries[i].label;
                obj.type_id = entries[i].type_id;
                obj.region_id = entries[i].region_id;
                obj.offset = entries[i].offset;
                obj.generation = entries[i].generation;
                obj.state = state;

                auto it = type_id_to_index_.find(obj.type_id);
                if (it != type_id_to_index_.end()) {
                    obj.type_name = types_[it->second].name;
                }

                return obj;
            }
        }
        return std::nullopt;
    };

    // Search in header
    uint32_t count = header_->object_count.load(std::memory_order_acquire);
    auto* entries = reinterpret_cast<const ObjectEntry*>(
        static_cast<const char*>(header_shm_.data()) + header_->object_dir_offset);

    if (auto found = search_entries(entries, count)) {
        return ObjectView(*this, *found);
    }

    // Search in overflow regions
    for (const auto& [id, shm] : overflow_shms_) {
        auto* desc = static_cast<const MetadataOverflowDescriptor*>(shm.data());
        uint32_t overflow_count = desc->object_entry_count.load(std::memory_order_acquire);
        auto* overflow_entries = reinterpret_cast<const ObjectEntry*>(
            static_cast<const char*>(shm.data()) + desc->object_entry_offset);

        if (auto found = search_entries(overflow_entries, overflow_count)) {
            return ObjectView(*this, *found);
        }
    }

    return ObjectView();
}

ObjectView Observer::get(const ObservedObject& obj) {
    return ObjectView(*this, obj);
}

const ObservedType* Observer::get_type(uint32_t type_id) const {
    auto it = type_id_to_index_.find(type_id);
    if (it != type_id_to_index_.end()) {
        return &types_[it->second];
    }
    return nullptr;
}

void* Observer::get_object_data(uint64_t region_id, uint64_t offset) {
    auto it = region_shms_.find(region_id);
    if (it == region_shms_.end()) {
        return nullptr;
    }
    return static_cast<char*>(it->second.data()) + offset;
}

void Observer::load_types() {
    types_.clear();
    type_id_to_index_.clear();

    if (!header_) return;

    uint32_t type_count = header_->type_count.load(std::memory_order_acquire);
    auto* type_entries = reinterpret_cast<const TypeEntry*>(
        static_cast<const char*>(header_shm_.data()) + header_->type_registry_offset);
    auto* field_entries = reinterpret_cast<const FieldEntry*>(
        static_cast<const char*>(header_shm_.data()) + header_->field_entries_offset);

    for (uint32_t i = 0; i < type_count; ++i) {
        const TypeEntry& te = type_entries[i];

        ObservedType type;
        type.type_id = te.type_id;
        type.name = te.name;
        type.size = te.size;
        type.alignment = te.alignment;

        // Load fields
        size_t field_start = (te.fields_offset - header_->field_entries_offset) / sizeof(FieldEntry);
        for (uint32_t j = 0; j < te.field_count; ++j) {
            type.fields.push_back(field_entries[field_start + j]);
        }

        type_id_to_index_[type.type_id] = types_.size();
        types_.push_back(std::move(type));
    }
}

void Observer::load_regions() {
    if (!header_) return;

    uint64_t region_id = header_->first_region_id.load(std::memory_order_acquire);

    while (region_id != 0) {
        // Skip if already loaded
        if (region_shms_.find(region_id) != region_shms_.end()) {
            // Get next region ID from existing region
            auto* desc = static_cast<RegionDescriptor*>(region_shms_[region_id].data());
            region_id = desc->next_region_id.load(std::memory_order_acquire);
            continue;
        }

        // Load new region
        std::string shm_name = detail::make_region_shm_name(session_name_, region_id);
        detail::SharedMemory shm;
        if (!shm.open(shm_name)) {
            break;  // Can't load region
        }

        auto* desc = static_cast<RegionDescriptor*>(shm.data());
        if (desc->magic != REGION_MAGIC) {
            break;  // Invalid region
        }

        uint64_t next_id = desc->next_region_id.load(std::memory_order_acquire);
        region_shms_[region_id] = std::move(shm);
        region_id = next_id;
    }
}

void Observer::load_overflow_regions() {
    if (!header_) return;

    uint64_t overflow_id = header_->first_overflow_region_id.load(std::memory_order_acquire);

    while (overflow_id != 0) {
        // Skip if already loaded
        if (overflow_shms_.find(overflow_id) != overflow_shms_.end()) {
            // Get next overflow ID from existing region
            auto* desc = static_cast<MetadataOverflowDescriptor*>(overflow_shms_[overflow_id].data());
            overflow_id = desc->next_region_id.load(std::memory_order_acquire);
            continue;
        }

        // Load new overflow region
        std::string shm_name = detail::make_overflow_shm_name(session_name_, overflow_id);
        detail::SharedMemory shm;
        if (!shm.open(shm_name)) {
            break;  // Can't load overflow region
        }

        auto* desc = static_cast<MetadataOverflowDescriptor*>(shm.data());
        if (desc->magic != OVERFLOW_MAGIC) {
            break;  // Invalid overflow region
        }

        uint64_t next_id = desc->next_region_id.load(std::memory_order_acquire);
        overflow_shms_[overflow_id] = std::move(shm);
        overflow_id = next_id;
    }
}

} // namespace memglass
