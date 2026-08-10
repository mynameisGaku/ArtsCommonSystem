// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"
#include "gameframework/CinematicsDirector.h"
#include "gameframework/cinetimeline/CCinematicTimelineDocument.h"
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"

#include <cmath>
#include <limits>
#include <new>

using namespace acs;
using namespace acs::game;
using namespace acs::game::cinetimeline;

namespace {
class CSwitchableFailAllocator final : public IAllocator {
public:
    explicit CSwitchableFailAllocator(IAllocator& backing) noexcept : m_Backing(&backing)
    {
    }

    void SetFailing(bool failing) noexcept
    {
        m_Failing = failing;
    }

    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        return m_Failing ? nullptr : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override
    {
        m_Backing->Free(pointer);
    }

private:
    IAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

class CDefaultAllocatorScope final {
public:
    explicit CDefaultAllocatorScope(IAllocator& allocator) noexcept : m_Previous(&DefaultAllocator())
    {
        SetDefaultAllocator(&allocator);
    }

    ~CDefaultAllocatorScope() noexcept
    {
        SetDefaultAllocator(m_Previous);
    }

private:
    IAllocator* m_Previous = nullptr;
};

struct FCapture {
    u32 event_ids[8]{};
    u32 event_count = 0u;
    FVec2 camera_target{};
    f32 camera_zoom = 0.0f;
    f32 camera_duration = 0.0f;
    u32 camera_count = 0u;
};

void OnEvent(void* user, u32 event_id) noexcept {
    FCapture& capture = *static_cast<FCapture*>(user);
    capture.event_ids[capture.event_count++] = event_id;
}

void OnCamera(void* user, FVec2 target, f32 zoom, f32 duration) noexcept {
    FCapture& capture = *static_cast<FCapture*>(user);
    capture.camera_target = target;
    capture.camera_zoom = zoom;
    capture.camera_duration = duration;
    ++capture.camera_count;
}

FCinematicTimelineKeyframe MakeKey(ECinematicTimelineKeyKind kind, f32 time_sec) noexcept {
    FCinematicTimelineKeyframe keyframe;
    keyframe.kind = kind;
    keyframe.time_sec = time_sec;
    return keyframe;
}

u32 BakeEventId(const FCinematicTimelineKeyframe& keyframe) noexcept {
    FCapture capture;
    CCinematicTimelineDocument document;
    CCinematicsDirector director;
    director.SetEventCallback(&OnEvent, &capture);
    if (!document.TryAdd(keyframe) || !document.TryBakeTo(director)) return 0u;
    director.Play();
    director.Tick(1.0f);
    return capture.event_count == 1u ? capture.event_ids[0] : 0u;
}
}

ACS_TEST(CinematicTimelineDocument, ValidatesActivePayloadsAndInactiveValues)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 positive_inf = std::numeric_limits<f32>::infinity();
    const f32 negative_inf = -positive_inf;
    CCinematicTimelineDocument document;

    FCinematicTimelineKeyframe camera = MakeKey(ECinematicTimelineKeyKind::CameraCut, 0.0f);
    camera.camera_target.x = nan;
    EXPECT_FALSE(document.TryAdd(camera));
    camera.camera_target.x = positive_inf;
    EXPECT_FALSE(document.TryAdd(camera));
    camera.camera_target.x = 0.0f;
    camera.camera_target.z = negative_inf;
    EXPECT_FALSE(document.TryAdd(camera));

    FCinematicTimelineKeyframe fade = MakeKey(ECinematicTimelineKeyKind::FadeColor, 0.0f);
    fade.fade_start_color.x = nan;
    EXPECT_FALSE(document.TryAdd(fade));
    fade.fade_start_color.x = 0.0f;
    fade.fade_end_color.y = positive_inf;
    EXPECT_FALSE(document.TryAdd(fade));

    FCinematicTimelineKeyframe scale = MakeKey(ECinematicTimelineKeyKind::TimeScale, 0.0f);
    scale.time_scale = nan;
    EXPECT_FALSE(document.TryAdd(scale));
    scale.time_scale = positive_inf;
    EXPECT_FALSE(document.TryAdd(scale));
    scale.time_scale = -1.0f;
    EXPECT_FALSE(document.TryAdd(scale));

    FCinematicTimelineKeyframe spawn = MakeKey(ECinematicTimelineKeyKind::SpawnEffect, 0.0f);
    spawn.camera_target.y = negative_inf;
    EXPECT_FALSE(document.TryAdd(spawn));

    FCinematicTimelineKeyframe trigger = MakeKey(ECinematicTimelineKeyKind::TriggerCallback, 0.0f);
    trigger.camera_target.x = nan;
    trigger.fade_end_color.y = positive_inf;
    trigger.time_scale = negative_inf;
    EXPECT_TRUE(document.TryAdd(trigger));

    FCinematicTimelineKeyframe invalid_time = trigger;
    invalid_time.time_sec = nan;
    EXPECT_FALSE(document.TryAdd(invalid_time));
    invalid_time.time_sec = -1.0f;
    EXPECT_FALSE(document.TryAdd(invalid_time));
    invalid_time.time_sec = 0.0f;
    invalid_time.kind = static_cast<ECinematicTimelineKeyKind>(255u);
    EXPECT_FALSE(document.TryAdd(invalid_time));
}

ACS_TEST(CinematicTimelineDocument, PreservesStableOrderAndAtomicDuration)
{
    CCinematicTimelineDocument document;
    FCinematicTimelineKeyframe first = MakeKey(ECinematicTimelineKeyKind::TriggerCallback, 1.0f);
    first.event_id = 100u;
    FCinematicTimelineKeyframe second = first;
    second.event_id = 200u;
    FCinematicTimelineKeyframe third = first;
    third.event_id = 300u;
    u32 index = 0u;
    EXPECT_TRUE(document.TryAdd(first, &index));
    EXPECT_TRUE(document.TryAdd(second, &index));
    EXPECT_TRUE(document.TryAdd(third, &index));
    EXPECT_EQ(index, 2u);
    EXPECT_EQ(document.Keyframes()[0].event_id, 100u);
    EXPECT_EQ(document.Keyframes()[1].event_id, 200u);
    EXPECT_EQ(document.Keyframes()[2].event_id, 300u);

    FCinematicTimelineKeyframe unchanged = document.Keyframes()[1];
    unchanged.event_id = 201u;
    EXPECT_TRUE(document.TryReplace(1u, unchanged, &index));
    EXPECT_EQ(index, 1u);
    EXPECT_EQ(document.Keyframes()[1].event_id, 201u);

    FCinematicTimelineKeyframe moved = document.Keyframes()[0];
    moved.time_sec = 2.0f;
    EXPECT_TRUE(document.TryReplace(0u, moved, &index));
    EXPECT_EQ(index, 2u);
    EXPECT_EQ(document.Keyframes()[0].event_id, 201u);
    EXPECT_EQ(document.Keyframes()[1].event_id, 300u);
    EXPECT_EQ(document.Keyframes()[2].event_id, 100u);

    EXPECT_FALSE(document.TryRemove(99u));
    EXPECT_TRUE(document.TrySetDurationSec(-1.0f));
    EXPECT_EQ(document.DurationSec(), CCinematicTimelineDocument::kMinDurationSec);
    EXPECT_EQ(document.Keyframes()[0].time_sec, CCinematicTimelineDocument::kMinDurationSec);
    EXPECT_EQ(document.Keyframes()[1].time_sec, CCinematicTimelineDocument::kMinDurationSec);
    EXPECT_EQ(document.Keyframes()[2].time_sec, CCinematicTimelineDocument::kMinDurationSec);
    EXPECT_EQ(document.Keyframes()[0].event_id, 201u);
    EXPECT_EQ(document.Keyframes()[1].event_id, 300u);
    EXPECT_EQ(document.Keyframes()[2].event_id, 100u);
}

ACS_TEST(CinematicTimelineDocument, BakeUsesCanonicalPayloadTags)
{
    CCinematicTimelineDocument document;
    FCinematicTimelineKeyframe camera = MakeKey(ECinematicTimelineKeyKind::CameraCut, 0.0f);
    camera.camera_target = FVec3{ 12.0f, 34.0f, 56.0f };
    FCinematicTimelineKeyframe fade = MakeKey(ECinematicTimelineKeyKind::FadeColor, 1.0f);
    fade.fade_end_color = FVec3{ 1.0f, 0.5f, 0.0f };
    FCinematicTimelineKeyframe scale = MakeKey(ECinematicTimelineKeyKind::TimeScale, 2.0f);
    scale.time_scale = 0.5f;
    FCinematicTimelineKeyframe spawn = MakeKey(ECinematicTimelineKeyKind::SpawnEffect, 3.0f);
    spawn.event_id = 0x12345678u;
    FCinematicTimelineKeyframe trigger = MakeKey(ECinematicTimelineKeyKind::TriggerCallback, 4.0f);
    trigger.event_id = 0xFFFFFFFFu;
    EXPECT_TRUE(document.TryAdd(camera));
    EXPECT_TRUE(document.TryAdd(fade));
    EXPECT_TRUE(document.TryAdd(scale));
    EXPECT_TRUE(document.TryAdd(spawn));
    EXPECT_TRUE(document.TryAdd(trigger));

    FCapture capture;
    CCinematicsDirector director;
    director.SetCameraCallback(&OnCamera, &capture);
    director.SetEventCallback(&OnEvent, &capture);
    EXPECT_TRUE(document.TryBakeTo(director));
    EXPECT_EQ(director.KeyframeCount(), 5u);
    director.Play();
    director.Tick(5.0f);
    EXPECT_EQ(capture.camera_count, 1u);
    EXPECT_EQ(capture.camera_target.x, 12.0f);
    EXPECT_EQ(capture.camera_target.y, 34.0f);
    EXPECT_EQ(capture.camera_zoom, 1.0f);
    EXPECT_EQ(capture.camera_duration, 0.0f);
    EXPECT_EQ(capture.event_count, 4u);
    EXPECT_EQ(capture.event_ids[0], 0x01FF8000u);
    EXPECT_EQ(capture.event_ids[1], 0x02100000u);
    EXPECT_EQ(capture.event_ids[2], 0x03345678u);
    EXPECT_EQ(capture.event_ids[3], 0x04FFFFFFu);
}

ACS_TEST(CinematicTimelineDocument, CoversAllCanonicalPayloadExamples)
{
    FCinematicTimelineKeyframe fade = MakeKey(ECinematicTimelineKeyKind::FadeColor, 0.0f);
    fade.fade_start_color = FVec3{ 0.9f, 0.8f, 0.7f };
    fade.fade_end_color = FVec3{ 0.0f, 0.0f, 0.0f };
    EXPECT_EQ(BakeEventId(fade), 0x01000000u);
    fade.fade_end_color = FVec3{ 1.0f, 1.0f, 1.0f };
    EXPECT_EQ(BakeEventId(fade), 0x01FFFFFFu);
    fade.fade_end_color = FVec3{ 0.1f, 0.2f, 0.3f };
    EXPECT_EQ(BakeEventId(fade), 0x011A334Du);

    FCinematicTimelineKeyframe scale = MakeKey(ECinematicTimelineKeyKind::TimeScale, 0.0f);
    const f32 scales[] = { 0.0f, 0.5f, 1.0f, 2.0f, 8.0f };
    const u32 scale_ids[] = { 0x02000000u, 0x02100000u, 0x02200000u, 0x02400000u, 0x02FFFFFFu };
    for (u32 i = 0u; i < 5u; ++i) {
        scale.time_scale = scales[i];
        EXPECT_EQ(BakeEventId(scale), scale_ids[i]);
    }

    FCinematicTimelineKeyframe spawn = MakeKey(ECinematicTimelineKeyKind::SpawnEffect, 0.0f);
    spawn.event_id = 0x12345678u;
    spawn.camera_target = FVec3{ 100.0f, 200.0f, 300.0f };
    EXPECT_EQ(BakeEventId(spawn), 0x03345678u);
    spawn.camera_target = FVec3{ -100.0f, -200.0f, -300.0f };
    EXPECT_EQ(BakeEventId(spawn), 0x03345678u);

    FCinematicTimelineKeyframe trigger = MakeKey(ECinematicTimelineKeyKind::TriggerCallback, 0.0f);
    trigger.event_id = 0xFFFFFFFFu;
    trigger.camera_target = FVec3{ std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f };
    EXPECT_EQ(BakeEventId(trigger), 0x04FFFFFFu);
}

ACS_TEST(CinematicTimelineDocument, AllocationFailuresAndDirectorStateAreAtomic)
{
    CSystemAllocator director_backing;
    CSwitchableFailAllocator director_allocator(director_backing);
    alignas(CCinematicsDirector) u8 director_storage[sizeof(CCinematicsDirector)];
    CCinematicsDirector* director = nullptr;
    {
        CDefaultAllocatorScope scope(director_allocator);
        director = ::new (director_storage) CCinematicsDirector();
    }
    {
        CSystemAllocator document_backing;
        CSwitchableFailAllocator document_allocator(document_backing);
        {
            CCinematicTimelineDocument document(document_allocator);
            FCinematicTimelineKeyframe keyframe = MakeKey(ECinematicTimelineKeyKind::TriggerCallback, 1.0f);
            keyframe.event_id = 1u;
            EXPECT_TRUE(document.TryAdd(keyframe));
            EXPECT_TRUE(document.TryBakeTo(*director));
        }
    }

    {
        CSystemAllocator document_backing;
        CSwitchableFailAllocator document_allocator(document_backing);
        CCinematicTimelineDocument empty_document(document_allocator);
        document_allocator.SetFailing(true);
        FCinematicTimelineKeyframe rejected = MakeKey(ECinematicTimelineKeyKind::TriggerCallback, 0.0f);
        EXPECT_FALSE(empty_document.TryAdd(rejected));
        EXPECT_EQ(empty_document.KeyframeCount(), 0u);
        document_allocator.SetFailing(false);
    }

    FTimelineKeyframe old_runtime;
    old_runtime.time_sec = 0.0f;
    old_runtime.kind = ETimelineTrackKind::FireEvent;
    old_runtime.payload.event.event_id = 99u;
    director->AddKeyframe(old_runtime);
    director->Play();
    director->Tick(0.1f);
    const f32 before_time = director->CurrentTime();
    const u32 before_count = director->KeyframeCount();
    {
        CSystemAllocator document_backing;
        CSwitchableFailAllocator document_allocator(document_backing);
        CCinematicTimelineDocument document(document_allocator);
        FCinematicTimelineKeyframe replacement = MakeKey(ECinematicTimelineKeyKind::TriggerCallback, 1.0f);
        replacement.event_id = 2u;
        EXPECT_TRUE(document.TryAdd(replacement));
        const u32 before_document_count = document.KeyframeCount();
        const FCinematicTimelineKeyframe before_document_key = document.Keyframes()[0];
        const f32 before_document_time = document.Keyframes()[0].time_sec;
        const u32 before_document_id = document.Keyframes()[0].event_id;
        const f32 before_document_duration = document.DurationSec();
        document_allocator.SetFailing(true);
        FCinematicTimelineKeyframe second = replacement;
        second.time_sec = 0.5f;
        second.event_id = 3u;
        EXPECT_FALSE(document.TryReplace(0u, second));
        EXPECT_FALSE(document.TrySetDurationSec(0.2f));
        EXPECT_EQ(document.KeyframeCount(), before_document_count);
        EXPECT_EQ(document.Keyframes()[0].kind, before_document_key.kind);
        EXPECT_EQ(document.Keyframes()[0].time_sec, before_document_time);
        EXPECT_EQ(document.Keyframes()[0].event_id, before_document_id);
        EXPECT_EQ(document.Keyframes()[0].camera_target.x, before_document_key.camera_target.x);
        EXPECT_EQ(document.Keyframes()[0].camera_target.y, before_document_key.camera_target.y);
        EXPECT_EQ(document.Keyframes()[0].camera_target.z, before_document_key.camera_target.z);
        EXPECT_EQ(document.Keyframes()[0].fade_start_color.x, before_document_key.fade_start_color.x);
        EXPECT_EQ(document.Keyframes()[0].fade_end_color.x, before_document_key.fade_end_color.x);
        EXPECT_EQ(document.Keyframes()[0].time_scale, before_document_key.time_scale);
        EXPECT_EQ(document.DurationSec(), before_document_duration);
        document_allocator.SetFailing(false);
    }

    CCinematicTimelineDocument bake_document;
    FCinematicTimelineKeyframe bake_key = MakeKey(ECinematicTimelineKeyKind::TriggerCallback, 0.5f);
    bake_key.event_id = 7u;
    EXPECT_TRUE(bake_document.TryAdd(bake_key));
    director_allocator.SetFailing(true);
    EXPECT_FALSE(bake_document.TryBakeTo(*director));
    EXPECT_EQ(director->CurrentTime(), before_time);
    EXPECT_EQ(director->KeyframeCount(), before_count);
    EXPECT_TRUE(director->IsPlaying());
    director_allocator.SetFailing(false);
    FCapture committed_capture;
    director->SetEventCallback(&OnEvent, &committed_capture);
    EXPECT_TRUE(bake_document.TryBakeTo(*director));
    EXPECT_FALSE(director->IsPlaying());
    EXPECT_EQ(director->CurrentTime(), 0.0f);
    EXPECT_EQ(director->KeyframeCount(), 1u);
    director->Play();
    director->Tick(0.5f);
    EXPECT_EQ(committed_capture.event_count, 1u);
    EXPECT_EQ(committed_capture.event_ids[0], 0x04000007u);
    director->Clear();
    director->~CCinematicsDirector();
}
