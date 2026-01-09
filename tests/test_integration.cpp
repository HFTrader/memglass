#include <gtest/gtest.h>
#include <memglass/memglass.hpp>
#include <memglass/observer.hpp>
#include <memglass/registry.hpp>
#include <cstring>
#include <thread>
#include <chrono>

using namespace memglass;

// Simple test types
struct SimpleStruct {
    int32_t x;
    int32_t y;
    double value;
};

struct ArrayStruct {
    int32_t values[4];
    char name[32];
};

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry::clear();

        // Register SimpleStruct type (using template version to also register typeid alias)
        TypeDescriptor simple_desc;
        simple_desc.name = "SimpleStruct";
        simple_desc.size = sizeof(SimpleStruct);
        simple_desc.alignment = alignof(SimpleStruct);
        simple_desc.fields = {
            {"x", offsetof(SimpleStruct, x), sizeof(int32_t),
             PrimitiveType::Int32, 0, 0, Atomicity::None, false},
            {"y", offsetof(SimpleStruct, y), sizeof(int32_t),
             PrimitiveType::Int32, 0, 0, Atomicity::None, false},
            {"value", offsetof(SimpleStruct, value), sizeof(double),
             PrimitiveType::Float64, 0, 0, Atomicity::None, false},
        };
        registry::register_type_for<SimpleStruct>(simple_desc);

        // Register ArrayStruct type
        TypeDescriptor array_desc;
        array_desc.name = "ArrayStruct";
        array_desc.size = sizeof(ArrayStruct);
        array_desc.alignment = alignof(ArrayStruct);
        array_desc.fields = {
            {"values", offsetof(ArrayStruct, values), sizeof(int32_t) * 4,
             PrimitiveType::Int32, 0, 4, Atomicity::None, false},
            {"name", offsetof(ArrayStruct, name), 32,
             PrimitiveType::Char, 0, 32, Atomicity::None, false},
        };
        registry::register_type_for<ArrayStruct>(array_desc);
    }

    void TearDown() override {
        memglass::shutdown();
        registry::clear();
    }
};

TEST_F(IntegrationTest, ProducerObserverBasic) {
    // Producer creates session
    ASSERT_TRUE(memglass::init("integration_test"));

    // Create an object
    auto* obj = memglass::create<SimpleStruct>("test_object");
    ASSERT_NE(obj, nullptr);

    // Set values
    obj->x = 42;
    obj->y = 100;
    obj->value = 3.14159;

    // Observer connects
    Observer observer("integration_test");
    ASSERT_TRUE(observer.connect());

    // Find the object
    auto view = observer.find("test_object");
    ASSERT_TRUE(static_cast<bool>(view));

    // Read values through observer
    int32_t x = view["x"].as<int32_t>();
    int32_t y = view["y"].as<int32_t>();
    double value = view["value"].as<double>();

    EXPECT_EQ(x, 42);
    EXPECT_EQ(y, 100);
    EXPECT_NEAR(value, 3.14159, 0.00001);
}

TEST_F(IntegrationTest, MultipleObjects) {
    ASSERT_TRUE(memglass::init("multi_object_test"));

    // Create multiple objects
    auto* obj1 = memglass::create<SimpleStruct>("object_1");
    auto* obj2 = memglass::create<SimpleStruct>("object_2");
    auto* obj3 = memglass::create<SimpleStruct>("object_3");

    ASSERT_NE(obj1, nullptr);
    ASSERT_NE(obj2, nullptr);
    ASSERT_NE(obj3, nullptr);

    obj1->x = 1;
    obj2->x = 2;
    obj3->x = 3;

    // Observer lists all objects
    Observer observer("multi_object_test");
    ASSERT_TRUE(observer.connect());

    auto objects = observer.objects();
    EXPECT_EQ(objects.size(), 3u);

    // Find each object
    auto view1 = observer.find("object_1");
    auto view2 = observer.find("object_2");
    auto view3 = observer.find("object_3");

    ASSERT_TRUE(static_cast<bool>(view1));
    ASSERT_TRUE(static_cast<bool>(view2));
    ASSERT_TRUE(static_cast<bool>(view3));

    EXPECT_EQ(view1["x"].as<int32_t>(), 1);
    EXPECT_EQ(view2["x"].as<int32_t>(), 2);
    EXPECT_EQ(view3["x"].as<int32_t>(), 3);
}

TEST_F(IntegrationTest, ObjectDestruction) {
    ASSERT_TRUE(memglass::init("destroy_test"));

    auto* obj = memglass::create<SimpleStruct>("temp_object");
    ASSERT_NE(obj, nullptr);
    obj->x = 999;

    Observer observer("destroy_test");
    ASSERT_TRUE(observer.connect());

    // Object should exist
    auto view = observer.find("temp_object");
    ASSERT_TRUE(static_cast<bool>(view));

    // Destroy the object
    memglass::destroy(obj);

    // Refresh observer
    observer.refresh();

    // Object should no longer be found
    auto view2 = observer.find("temp_object");
    EXPECT_FALSE(static_cast<bool>(view2));
}

TEST_F(IntegrationTest, ArrayFields) {
    ASSERT_TRUE(memglass::init("array_test"));

    auto* obj = memglass::create<ArrayStruct>("array_object");
    ASSERT_NE(obj, nullptr);

    obj->values[0] = 10;
    obj->values[1] = 20;
    obj->values[2] = 30;
    obj->values[3] = 40;
    std::strcpy(obj->name, "TestArray");

    Observer observer("array_test");
    ASSERT_TRUE(observer.connect());

    auto view = observer.find("array_object");
    ASSERT_TRUE(static_cast<bool>(view));

    // Read array elements
    EXPECT_EQ(view["values"][0].as<int32_t>(), 10);
    EXPECT_EQ(view["values"][1].as<int32_t>(), 20);
    EXPECT_EQ(view["values"][2].as<int32_t>(), 30);
    EXPECT_EQ(view["values"][3].as<int32_t>(), 40);
}

TEST_F(IntegrationTest, ObserverRefresh) {
    ASSERT_TRUE(memglass::init("refresh_test"));

    auto* obj = memglass::create<SimpleStruct>("refresh_object");
    ASSERT_NE(obj, nullptr);
    obj->x = 1;

    Observer observer("refresh_test");
    ASSERT_TRUE(observer.connect());

    auto view = observer.find("refresh_object");
    ASSERT_TRUE(static_cast<bool>(view));
    EXPECT_EQ(view["x"].as<int32_t>(), 1);

    // Producer updates value
    obj->x = 2;

    // Observer should see new value (no refresh needed for direct reads)
    EXPECT_EQ(view["x"].as<int32_t>(), 2);
}

TEST_F(IntegrationTest, SessionMetadata) {
    ASSERT_TRUE(memglass::init("metadata_test"));

    Observer observer("metadata_test");
    ASSERT_TRUE(observer.connect());

    EXPECT_GT(observer.producer_pid(), 0u);
    EXPECT_GT(observer.start_timestamp(), 0u);
    EXPECT_GE(observer.sequence(), 0u);
}

TEST_F(IntegrationTest, InvalidSession) {
    Observer observer("nonexistent_session");
    EXPECT_FALSE(observer.connect());
}

TEST_F(IntegrationTest, ObjectNotFound) {
    ASSERT_TRUE(memglass::init("notfound_test"));

    Observer observer("notfound_test");
    ASSERT_TRUE(observer.connect());

    auto view = observer.find("does_not_exist");
    EXPECT_FALSE(static_cast<bool>(view));
}

TEST_F(IntegrationTest, ConcurrentProducerObserver) {
    ASSERT_TRUE(memglass::init("concurrent_test"));

    auto* obj = memglass::create<SimpleStruct>("concurrent_object");
    ASSERT_NE(obj, nullptr);

    std::atomic<bool> stop{false};
    std::atomic<bool> reader_ready{false};
    std::atomic<int> updates{0};
    std::atomic<int> reads{0};

    // Observer thread continuously reads
    std::thread observer_thread([&]() {
        Observer obs("concurrent_test");
        if (!obs.connect()) {
            stop = true;
            reader_ready = true;
            return;
        }

        auto view = obs.find("concurrent_object");
        if (!static_cast<bool>(view)) {
            stop = true;
            reader_ready = true;
            return;
        }

        reader_ready = true;  // Signal that we're ready to read

        while (!stop) {
            int32_t x = view["x"].as<int32_t>();
            int32_t y = view["y"].as<int32_t>();
            (void)x;
            (void)y;
            reads++;
        }
    });

    // Wait for reader to be ready before starting producer
    while (!reader_ready) {
        std::this_thread::yield();
    }

    // Producer thread continuously updates
    for (int i = 0; i < 1000 && !stop; ++i) {
        obj->x = i;
        obj->y = i;
        updates++;
    }

    stop = true;
    observer_thread.join();

    EXPECT_EQ(updates, 1000);
    EXPECT_GT(reads, 0);
}

TEST_F(IntegrationTest, MetadataOverflow) {
    // Test that creating more objects than initial capacity triggers overflow
    // and all objects remain observable

    Config cfg;
    cfg.max_objects = 10;  // Set a small limit to trigger overflow quickly

    ASSERT_TRUE(memglass::init("overflow_test", cfg));

    // Create more objects than the initial capacity
    const int num_objects = 25;
    std::vector<SimpleStruct*> objects;
    objects.reserve(num_objects);

    for (int i = 0; i < num_objects; ++i) {
        std::string label = "overflow_obj_" + std::to_string(i);
        auto* obj = memglass::create<SimpleStruct>(label);
        ASSERT_NE(obj, nullptr) << "Failed to create object " << i;
        obj->x = i;
        obj->y = i * 10;
        obj->value = static_cast<double>(i) * 1.5;
        objects.push_back(obj);
    }

    // Observer should see all objects including those in overflow regions
    Observer observer("overflow_test");
    ASSERT_TRUE(observer.connect());

    // Verify all objects are observable
    auto observed_objects = observer.objects();
    EXPECT_EQ(observed_objects.size(), static_cast<size_t>(num_objects));

    // Verify each object can be found and has correct values
    for (int i = 0; i < num_objects; ++i) {
        std::string label = "overflow_obj_" + std::to_string(i);
        auto view = observer.find(label);
        ASSERT_TRUE(static_cast<bool>(view)) << "Failed to find object: " << label;

        int32_t x = view["x"].as<int32_t>();
        int32_t y = view["y"].as<int32_t>();
        double value = view["value"].as<double>();

        EXPECT_EQ(x, i) << "Incorrect x for object " << i;
        EXPECT_EQ(y, i * 10) << "Incorrect y for object " << i;
        EXPECT_NEAR(value, static_cast<double>(i) * 1.5, 0.0001) << "Incorrect value for object " << i;
    }
}
