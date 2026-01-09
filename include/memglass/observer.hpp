#pragma once

#include "types.hpp"
#include "detail/shm.hpp"
#include "detail/seqlock.hpp"

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace memglass {

// Forward declarations
class Observer;
class ObjectView;
class FieldProxy;

// Type information as seen by observer
struct ObservedType {
    uint32_t type_id;
    std::string name;
    uint32_t size;
    uint32_t alignment;
    std::vector<FieldEntry> fields;
};

// Object information as seen by observer
struct ObservedObject {
    std::string label;
    std::string type_name;
    uint32_t type_id;
    uint64_t region_id;
    uint64_t offset;
    uint64_t generation;
    ObjectState state;
};

// Field proxy for intuitive field access
class FieldProxy {
public:
    FieldProxy(ObjectView& obj, const FieldEntry* field, void* data);

    // Implicit conversion for reads
    template<typename T>
    operator T() const {
        return read<T>();
    }

    // Explicit read
    template<typename T>
    T read() const {
        if (!data_ || !field_) return T{};

        switch (field_->atomicity) {
            case Atomicity::Atomic:
                return read_atomic<T>();
            case Atomicity::Seqlock:
                return read_seqlock<T>();
            case Atomicity::Locked:
                return read_locked<T>();
            default:
                return read_direct<T>();
        }
    }

    // Assignment for writes
    template<typename T>
    FieldProxy& operator=(const T& value) {
        if (!data_ || !field_) return *this;

        switch (field_->atomicity) {
            case Atomicity::Atomic:
                write_atomic(value);
                break;
            case Atomicity::Seqlock:
                write_seqlock(value);
                break;
            case Atomicity::Locked:
                write_locked(value);
                break;
            default:
                write_direct(value);
                break;
        }
        return *this;
    }

    // Nested field access
    FieldProxy operator[](std::string_view name) const;
    FieldProxy operator[](size_t index) const;

    // Explicit type conversion
    template<typename T>
    T as() const { return read<T>(); }

    // Try-read for seqlock (non-blocking)
    template<typename T>
    std::optional<T> try_get() const {
        if (!data_ || !field_) return std::nullopt;
        if (field_->atomicity != Atomicity::Seqlock) {
            return read<T>();
        }
        auto* guarded = reinterpret_cast<const Guarded<T>*>(data_);
        return guarded->try_read();
    }

    // Unsafe direct read (bypasses atomicity)
    template<typename T>
    T unsafe() const {
        if (!data_) return T{};
        return *reinterpret_cast<const T*>(data_);
    }

    // Field info
    const FieldEntry* info() const { return field_; }

    // Check if valid
    explicit operator bool() const { return data_ != nullptr && field_ != nullptr; }

private:
    template<typename T>
    T read_direct() const {
        return *reinterpret_cast<const T*>(data_);
    }

    template<typename T>
    T read_atomic() const {
        auto* atomic_ptr = reinterpret_cast<const std::atomic<T>*>(data_);
        return atomic_ptr->load(std::memory_order_acquire);
    }

    template<typename T>
    T read_seqlock() const {
        auto* guarded = reinterpret_cast<const Guarded<T>*>(data_);
        return guarded->read();
    }

    template<typename T>
    T read_locked() const {
        auto* locked = reinterpret_cast<const Locked<T>*>(data_);
        return locked->read();
    }

    template<typename T>
    void write_direct(const T& value) {
        *reinterpret_cast<T*>(data_) = value;
    }

    template<typename T>
    void write_atomic(const T& value) {
        auto* atomic_ptr = reinterpret_cast<std::atomic<T>*>(data_);
        atomic_ptr->store(value, std::memory_order_release);
    }

    template<typename T>
    void write_seqlock(const T& value) {
        auto* guarded = reinterpret_cast<Guarded<T>*>(data_);
        guarded->write(value);
    }

    template<typename T>
    void write_locked(const T& value) {
        auto* locked = reinterpret_cast<Locked<T>*>(data_);
        locked->write(value);
    }

    ObjectView& obj_;
    const FieldEntry* field_;
    void* data_;
};

// View of an object for reading/writing fields
class ObjectView {
public:
    ObjectView() = default;
    ObjectView(Observer& observer, const ObservedObject& obj_info);

    // Field access via operator[]
    FieldProxy operator[](std::string_view field_name);
    FieldProxy operator[](const char* field_name) { return (*this)[std::string_view(field_name)]; }

    // Read entire object
    template<typename T>
    T as() const {
        if (!data_) return T{};
        T result;
        std::memcpy(&result, data_, sizeof(T));
        return result;
    }

    // Raw data access
    const void* data() const { return data_; }
    void* mutable_data() { return data_; }

    // Object info
    const ObservedObject& info() const { return obj_info_; }
    const ObservedType* type() const { return type_; }

    // Check if valid
    explicit operator bool() const { return data_ != nullptr; }

private:
    friend class FieldProxy;

    const FieldEntry* find_field(std::string_view name) const;

    Observer* observer_ = nullptr;
    ObservedObject obj_info_;
    const ObservedType* type_ = nullptr;
    void* data_ = nullptr;
};

// Observer - connects to a memglass session and reads data
class Observer {
public:
    explicit Observer(std::string_view session_name);
    ~Observer();

    // Connect to the session
    bool connect();

    // Disconnect
    void disconnect();

    // Refresh view of shared memory (detect new types/objects)
    void refresh();

    // Check if connected
    bool is_connected() const { return connected_; }

    // Producer info
    uint64_t producer_pid() const;
    uint64_t start_timestamp() const;

    // Sequence number (changes on structural modifications)
    uint64_t sequence() const;

    // Get all types
    const std::vector<ObservedType>& types() const { return types_; }

    // Get all objects
    std::vector<ObservedObject> objects() const;

    // Find object by label
    ObjectView find(std::string_view label);

    // Get object view
    ObjectView get(const ObservedObject& obj);

    // Get type by ID
    const ObservedType* get_type(uint32_t type_id) const;

    // Get pointer to object data
    void* get_object_data(uint64_t region_id, uint64_t offset);

private:
    std::string session_name_;
    bool connected_ = false;

    detail::SharedMemory header_shm_;
    TelemetryHeader* header_ = nullptr;

    std::unordered_map<uint64_t, detail::SharedMemory> region_shms_;
    std::unordered_map<uint64_t, detail::SharedMemory> overflow_shms_;
    std::vector<ObservedType> types_;
    std::unordered_map<uint32_t, size_t> type_id_to_index_;
    uint64_t last_sequence_ = 0;

    void load_types();
    void load_regions();
    void load_overflow_regions();
};

} // namespace memglass
