// SPDX-License-Identifier: Apache-2.0
// HelloPersistVerify - stub 撲滅 Batch 1 の本実装を検証するヘッドレスコンソール。
//
// 旧 stub だった以下の永続化/シリアライズを「保存→読込→一致」で round-trip 検証する:
//   1) FSettings::Save/Load          (INI、型 round-trip)
//   2) FLockstep::SaveToBuffer/Load  (.acsl バイナリ + CRC32 + checksum)
//   3) FInputRecorder::SaveToBuffer  (.acsr バイナリ + CRC32)
//   4) FProgression::Save/Load       (FSaveArchive バイナリ、XP + milestone 達成)
// GPU 不要・ヘッドレス。全項目 OK なら exit 0、1 つでも FAIL なら exit 1。
#include "gameframework/GameFramework.h"
#include "asset/AssetRegistry.h"

#include <cstdio>
#include <cstring>
#include <cmath>

using namespace acs;
using namespace acs::game;

namespace {

int g_fail = 0;
void Check(bool cond, const char* what) noexcept {
    std::printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond) g_fail = 1;
}

bool NearF(f32 a, f32 b) noexcept { return std::fabs(a - b) < 1e-4f; }

void TestSettings() noexcept {
    std::printf("[FSettings] INI 保存/読込 round-trip\n");
    const wchar_t* path = L"persist_settings.ini";
    {
        FSettings s;
        s.SetF32("audio.master", 0.8f);
        s.SetI32("display.width", 1920);
        s.SetBool("display.vsync", true);
        s.SetString("locale", "ja");
        Check(s.Save(path).IsOk(), "Save() 成功");
    }
    FSettings s2;
    Check(s2.Load(path).IsOk(), "Load() 成功");
    Check(s2.Count() == 4, "件数 4");
    Check(NearF(s2.GetF32("audio.master", -1.0f), 0.8f), "f32 audio.master=0.8");
    Check(s2.GetI32("display.width", -1) == 1920, "i32 display.width=1920");
    Check(s2.GetBool("display.vsync", false) == true, "bool display.vsync=true");
    Check(std::strcmp(s2.GetString("locale", ""), "ja") == 0, "string locale=ja");
    Check(NearF(s2.GetF32("missing", 3.0f), 3.0f), "未設定キーは default");
}

void TestLockstep() noexcept {
    std::printf("[FLockstep] .acsl バイナリ round-trip\n");
    FLockstep ls;
    ls.Init(ENetMode::Local, 60);
    for (u32 i = 0; i < 5; ++i) {
        InputFrame f;
        f.tick = i; f.player_id = i & 1u; f.buttons = static_cast<u8>(0x10 + i);
        f.axis = FVec2{ static_cast<f32>(i) * 0.25f, -static_cast<f32>(i) * 0.5f };
        ls.RecordInput(f);
    }
    const u64 csum = ls.ComputeChecksum();

    u8 buf[1024]; u32 written = 0;
    Check(ls.SaveToBuffer(buf, sizeof(buf), written).IsOk(), "SaveToBuffer 成功");
    Check(written > 0 && written < sizeof(buf), "written サイズ妥当");

    FLockstep ls2;
    ls2.Init(ENetMode::Local, 60);
    Check(ls2.LoadFromBuffer(buf, written).IsOk(), "LoadFromBuffer 成功");
    Check(ls2.InputCount() == 5, "frame 数 5");
    Check(ls2.TickRateHz() == 60, "tick_rate 60");
    Check(ls2.ComputeChecksum() == csum, "checksum 一致 (bit 完全一致)");

    // 中身も確認 (replay で 2 番目の frame を取り出す)。
    ls2.StartReplay();
    InputFrame out;
    bool got = false;
    for (u32 t = 0; t < 5; ++t) { if (ls2.ConsumeInput(t, t & 1u, out) && t == 2) { got = true; break; } }
    Check(got && out.buttons == 0x12 && NearF(out.axis.x, 0.5f), "frame[2] のボタン/軸一致");

    // 改竄検知: CRC を壊すと load 失敗。
    buf[written - 1] ^= 0xFF;
    FLockstep ls3;
    Check(ls3.LoadFromBuffer(buf, written).IsErr(), "CRC 改竄を検知 (load 失敗)");
}

void TestInputRecorder() noexcept {
    std::printf("[FInputRecorder] .acsr バイナリ round-trip\n");
    FInputRecorder rec;
    rec.StartRecording(120);
    for (u32 i = 0; i < 4; ++i) {
        InputSample s;
        s.tick = i;
        s.key_codes_changed[0] = static_cast<u8>(65 + i);  // 'A','B','C','D'
        s.key_states[0] = 1;
        s.mouse_pos = FVec2{ static_cast<f32>(i) * 10.0f, static_cast<f32>(i) * 20.0f };
        s.mouse_button_states = static_cast<u8>(i);
        rec.Capture(s);
    }
    rec.StopRecording();

    u8 buf[1024]; u32 written = 0;
    Check(rec.SaveToBuffer(buf, sizeof(buf), written).IsOk(), "SaveToBuffer 成功");

    FInputRecorder rec2;
    Check(rec2.LoadFromBuffer(buf, written).IsOk(), "LoadFromBuffer 成功");
    Check(rec2.SampleCount() == 4, "sample 数 4");
    Check(rec2.TickRateHz() == 120, "tick_rate 120");

    rec2.StartReplay();
    InputSample out;
    bool got = rec2.ConsumeSample(2, out);
    Check(got && out.key_codes_changed[0] == 67 && NearF(out.mouse_pos.y, 40.0f),
          "sample[2] のキー/マウス一致");
}

void TestProgression() noexcept {
    std::printf("[FProgression] FSaveArchive バイナリ round-trip\n");
    const wchar_t* path = L"persist_progress.sav";
    const MilestoneDef defs[3] = {
        { "ms.lv5",  "Lv.5",   31,    "content.weapon_b" },
        { "ms.lv10", "Lv.10",  1023,  "content.area_2"   },
        { "ms.vet",  "Veteran",16383, "content.title_x"  },
    };
    {
        FProgression p;
        for (auto& d : defs) p.RegisterMilestone(d);
        p.AwardXp(50);   // ms.lv5 達成 (>=31)
        Check(p.CurrentXp() == 50, "XP=50");
        Check(p.IsMilestoneAchieved("ms.lv5"), "ms.lv5 達成");
        Check(!p.IsMilestoneAchieved("ms.lv10"), "ms.lv10 未達成");
        Check(p.Save(path).IsOk(), "Save() 成功");
    }
    FProgression p2;
    for (auto& d : defs) p2.RegisterMilestone(d);   // 同 def を登録してから Load
    Check(p2.Load(path).IsOk(), "Load() 成功");
    Check(p2.CurrentXp() == 50, "XP=50 復元");
    Check(p2.IsMilestoneAchieved("ms.lv5"), "ms.lv5 達成 復元");
    Check(!p2.IsMilestoneAchieved("ms.lv10"), "ms.lv10 未達成 復元");
    Check(p2.AchievedCount() == 1, "達成数 1");
}

void TestAssetBundle() noexcept {
    std::printf("[FAssetBundle] FAssetRegistry 実ロード\n");
    FAssetRegistry reg;
    reg.RegisterDefaultLoaders();
    const char* fname = "bundle_test.dat";
    {
        std::FILE* f = std::fopen(fname, "wb");
        Check(f != nullptr, "テストファイル作成");
        if (f) { std::fwrite("ACS-BUNDLE-OK", 1, 13, f); std::fclose(f); }
    }
    FAssetBundle bundle;
    bundle.Add(fname);
    bundle.Add("does_not_exist_xyz.dat");          // 失敗パス
    bundle.BeginLoad(reg);
    Check(bundle.AssetCount() == 2, "登録 2 件");
    Check(bundle.IsLoaded(), "IsLoaded (成功/失敗とも完了)");
    Check(bundle.LoadedCount() == 1, "成功 1 件");
    Check(bundle.HasFailed(), "欠落ファイルを Failed 検知");
    Check(bundle.GetAsset(0).Get() != nullptr, "GetAsset(0) で実体保持");
    Check(bundle.FindAsset(fname).Get() != nullptr, "FindAsset で取得");
    Check(bundle.GetAsset(1).Get() == nullptr, "失敗 entry は空 TRc");
    bundle.Unload();
    Check(bundle.AssetCount() == 0, "Unload で空に");
    std::remove(fname);
}

} // namespace

int main() {
    std::printf("=== Persist/Serialize round-trip 検証 (stub 撲滅 Batch 1/2) ===\n");
    TestSettings();
    TestLockstep();
    TestInputRecorder();
    TestProgression();
    TestAssetBundle();
    std::printf("=== %s ===\n", g_fail == 0 ? "ALL PASS" : "FAILED");
    return g_fail;
}
