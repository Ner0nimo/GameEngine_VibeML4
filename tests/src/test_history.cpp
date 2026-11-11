#include <gtest/gtest.h>

#include "ignis/ecs/components.h"
#include "ignis/scene/history.h"
#include "ignis/scene/scene.h"

#include <string>

using namespace ignis;

namespace {

Scene make_scene() {
    Scene scene("hist");
    register_builtin_components(scene.world());
    EntityId e = scene.world().create_entity("box");
    scene.world().emplace<TransformComponent>(e)->position = {1.f, 2.f, 3.f};
    return scene;
}

f32 pos_x(Scene& scene, EntityId e) {
    return scene.world().get<TransformComponent>(e)->position.x;
}

} // namespace

TEST(SceneHistory, TransactionUndoRedo) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    const EntityId orig = scene.world().all_entities().front();

    h.begin("move box");
    scene.world().get<TransformComponent>(orig)->position = {9.f, 9.f, 9.f};
    h.commit();
    ASSERT_TRUE(h.can_undo());
    EXPECT_EQ(h.undo_label(), "move box");
    EXPECT_EQ(h.undo_depth(), 1u);
    EXPECT_FALSE(h.can_redo());

    ASSERT_TRUE(h.undo());
    EXPECT_EQ(pos_x(scene, orig), 1.f);
    EXPECT_FALSE(h.can_undo());
    ASSERT_TRUE(h.can_redo());
    EXPECT_EQ(h.redo_label(), "move box");

    ASSERT_TRUE(h.redo());
    EXPECT_EQ(pos_x(scene, orig), 9.f);
    // Entity id survives undo/redo (load_json preserves ids).
    EXPECT_TRUE(scene.world().alive(orig));
}

TEST(SceneHistory, CreateEntityUndoRemovesRedoRestoresSameId) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    h.begin("create");
    const EntityId e = scene.world().create_entity("new");
    h.commit();
    EXPECT_EQ(scene.world().entity_count(), 2u);

    ASSERT_TRUE(h.undo());
    EXPECT_EQ(scene.world().entity_count(), 1u);
    EXPECT_FALSE(scene.world().alive(e));

    ASSERT_TRUE(h.redo());
    EXPECT_EQ(scene.world().entity_count(), 2u);
    EXPECT_TRUE(scene.world().alive(e)); // same id comes back
    EXPECT_EQ(scene.world().name_of(e), "new");
}

TEST(SceneHistory, NoOpTransactionIsDiscarded) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    h.begin("nothing happened");
    h.commit();
    EXPECT_FALSE(h.can_undo());
    EXPECT_EQ(h.undo_depth(), 0u);
    // commit with no open transaction is a no-op too
    h.commit();
    EXPECT_FALSE(h.can_undo());
}

TEST(SceneHistory, SameNameCoalescesIntoOneEntry) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    const EntityId e = scene.world().all_entities().front();

    h.begin("drag");
    scene.world().get<TransformComponent>(e)->position.x = 2.f;
    h.begin("drag"); // still open, same name: original "before" is kept
    scene.world().get<TransformComponent>(e)->position.x = 3.f;
    h.begin("drag");
    scene.world().get<TransformComponent>(e)->position.x = 4.f;
    h.commit();

    EXPECT_EQ(h.undo_depth(), 1u);
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(pos_x(scene, e), 1.f); // whole drag reverted at once
}

TEST(SceneHistory, DifferentNameCommitsPreviousTransaction) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    const EntityId e = scene.world().all_entities().front();

    h.begin("a");
    scene.world().get<TransformComponent>(e)->position.x = 2.f;
    h.begin("b"); // auto-commits "a"
    scene.world().get<TransformComponent>(e)->position.x = 3.f;
    h.commit();

    EXPECT_EQ(h.undo_depth(), 2u);
    EXPECT_EQ(h.undo_label(), "b");
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(pos_x(scene, e), 2.f);
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(pos_x(scene, e), 1.f);
    EXPECT_FALSE(h.can_undo());
}

TEST(SceneHistory, NewChangeClearsRedoStack) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    const EntityId e = scene.world().all_entities().front();

    h.begin("a");
    scene.world().get<TransformComponent>(e)->position.x = 2.f;
    h.commit();
    ASSERT_TRUE(h.undo());
    ASSERT_TRUE(h.can_redo());

    h.begin("b");
    scene.world().get<TransformComponent>(e)->position.x = 5.f;
    h.commit();
    EXPECT_FALSE(h.can_redo());
    EXPECT_EQ(h.undo_depth(), 1u);
}

TEST(SceneHistory, CapacityTrimsOldestEntries) {
    Scene scene = make_scene();
    SceneHistory h(scene, 3);
    const EntityId e = scene.world().all_entities().front();

    for (int i = 2; i <= 6; ++i) {
        h.begin("step " + std::to_string(i));
        scene.world().get<TransformComponent>(e)->position.x = static_cast<f32>(i);
        h.commit();
    }
    EXPECT_EQ(h.undo_depth(), 3u);
    EXPECT_EQ(h.undo_label(), "step 6");
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(pos_x(scene, e), 5.f);
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(pos_x(scene, e), 4.f);
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(pos_x(scene, e), 3.f); // steps 2 and 3 were trimmed
    EXPECT_FALSE(h.can_undo());
}

TEST(SceneHistory, EmptyUndoRedoReturnFalseAndEmptyLabels) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    EXPECT_FALSE(h.undo());
    EXPECT_FALSE(h.redo());
    EXPECT_TRUE(h.undo_label().empty());
    EXPECT_TRUE(h.redo_label().empty());
    EXPECT_EQ(h.redo_depth(), 0u);
}

TEST(SceneHistory, AbortDiscardsOpenTransactionWithoutReverting) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    const EntityId e = scene.world().all_entities().front();

    h.begin("abort me");
    scene.world().get<TransformComponent>(e)->position.x = 42.f;
    h.abort();
    EXPECT_FALSE(h.can_undo());
    h.commit(); // nothing open: no-op
    EXPECT_FALSE(h.can_undo());
    // abort records nothing; the scene mutation itself is the caller's business
    EXPECT_EQ(pos_x(scene, e), 42.f);
}

TEST(SceneHistory, ClearResetsBothStacks) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    h.begin("x");
    scene.world().create_entity("tmp");
    h.commit();
    ASSERT_TRUE(h.can_undo());
    h.clear();
    EXPECT_FALSE(h.can_undo());
    EXPECT_FALSE(h.can_redo());
    EXPECT_FALSE(h.undo());
}

TEST(SceneHistory, DeleteEntityUndoRestoresWithComponents) {
    Scene scene = make_scene();
    SceneHistory h(scene);
    const EntityId e = scene.world().all_entities().front();

    h.begin("delete box");
    scene.world().destroy_entity(e);
    h.commit();
    ASSERT_TRUE(h.undo());
    ASSERT_TRUE(scene.world().alive(e));
    EXPECT_EQ(scene.world().name_of(e), "box");
    ASSERT_TRUE(scene.world().has<TransformComponent>(e));
    EXPECT_EQ(pos_x(scene, e), 1.f);
}
