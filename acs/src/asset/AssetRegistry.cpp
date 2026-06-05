// SPDX-License-Identifier: Apache-2.0
// FAssetRegistry 実装
#include "asset/AssetRegistry.h"
#include "asset/BinaryAsset.h"
#include "asset/TextAsset.h"
#include "asset/ImageAsset.h"
#include "asset/AudioAsset.h"
#include "asset/MeshAsset.h"
#include "platform/FileSystem.h"
#include "threading/ScopedLock.h"
#include "memory/UniquePtr.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/**
 * wchar_t パスから拡張子を小文字 ASCII で取り出す。
 *
 * @details
 * 末尾から '.' を探し、見つかった拡張子を小文字化して out にコピーする。'.' が
 * 無ければ空文字列。ASCII 範囲外の文字は '?' に置換する (一般的でないため扱わない)。
 * @param path 拡張子を取り出す対象のパス。
 * @param out 拡張子を書き込む NUL 終端バッファ。
 * @param cap out バッファの容量 (バイト数)。
 */
void ExtractExtensionAscii(const wchar_t* path, char* out, usize cap) noexcept {
    // 末尾から '.' を探す
    usize len = 0;
    while (path[len]) ++len;
    isize dot = -1;
    for (isize i = static_cast<isize>(len) - 1; i >= 0; --i) {
        if (path[i] == L'.') { dot = i; break; }
        if (path[i] == L'\\' || path[i] == L'/') break;  // ディレクトリ区切りで終了
    }
    if (dot < 0) { if (cap) out[0] = 0; return; }
    // 拡張子部分をコピーして小文字化
    usize w = 0;
    for (usize i = static_cast<usize>(dot) + 1; i < len && w + 1 < cap; ++i) {
        wchar_t c = path[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        out[w++] = (c < 128) ? static_cast<char>(c) : '?';
    }
    out[w] = 0;
}

/**
 * 2 つの NUL 終端 ASCII 文字列が等しいかを返す。
 *
 * @param a 比較する文字列 1。
 * @param b 比較する文字列 2。
 * @return 完全一致すれば true。
 */
bool StrEqAscii(const char* a, const char* b) noexcept {
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == 0 && *b == 0;
}

/**
 * パスから FAssetId を作る (wchar_t 列をバイト列としてハッシュ)。
 *
 * @param path ハッシュ元のパス。
 * @return パスから生成した FAssetId。
 */
FAssetId MakeIdFromPath(const wchar_t* path) noexcept {
    usize len = 0;
    while (path[len]) ++len;
    return MakeAssetId(FStringView(reinterpret_cast<const char*>(path), len * sizeof(wchar_t)));
}

} // namespace

void FAssetRegistry::RegisterLoader(IAssetLoader* loader) noexcept {
    if (!loader) return;
    FScopedLock lk(m_Lock);
    m_Loaders.PushBack(loader);
}

IAssetLoader* FAssetRegistry::FindLoader(const wchar_t* path) noexcept {
    char ext[32]{};
    ExtractExtensionAscii(path, ext, sizeof(ext));
    IAssetLoader* fallback = nullptr;
    for (usize i = 0; i < m_Loaders.Size(); ++i) {
        const char* const e = m_Loaders[i]->Extension();
        if (StrEqAscii(e, "*")) fallback = m_Loaders[i];
        if (StrEqAscii(e, ext)) return m_Loaders[i];
    }
    return fallback;  // 拡張子マッチなければ "*" のフォールバックを返す
}

TResult<TSharedPtr<Asset>> FAssetRegistry::Load(const wchar_t* path) noexcept {
    if (!path) return ACS_ERR(Asset, 1, "FAssetRegistry::Load: null path");

    const FAssetId id = MakeIdFromPath(path);

    // キャッシュにあればそのまま返す
    {
        FScopedLock lk(m_Lock);
        const TSharedPtr<Asset>* hit = m_Cache.Find(id);
        if (hit && hit->Get()) return TResult<TSharedPtr<Asset>>(OkInit, *hit);
    }

    // 拡張子から適切なローダを選ぶ
    IAssetLoader* loader = nullptr;
    {
        FScopedLock lk(m_Lock);
        loader = FindLoader(path);
    }
    if (!loader) return ACS_ERR(Asset, 2, "no loader for this asset path");

    // ファイルを読み込む
    auto bytes_r = FileSystem::ReadAllBytes(path);
    if (bytes_r.IsErr()) return Err<TSharedPtr<Asset>>(bytes_r.Error());

    // ローダ呼び出し
    auto asset_r = loader->LoadFromBytes(id, bytes_r.Value());
    if (asset_r.IsErr()) return Err<TSharedPtr<Asset>>(asset_r.Error());

    TSharedPtr<Asset> a = Move(asset_r.Value());
    if (!a.Get())  // ローダが OkInit で null を返した場合 (alloc 失敗等) の null-deref 回避
        return ACS_ERR(Asset, 3, "loader returned null asset");
    a->SetId(id);
    a->SetState(EAssetState::Ready);

    // キャッシュに登録
    {
        FScopedLock lk(m_Lock);
        m_Cache.Insert(id, a);
    }
    return TResult<TSharedPtr<Asset>>(OkInit, Move(a));
}

namespace {
/** 非同期ロードワーカーに渡すジョブ引数 (heap 確保し所有権をワーカーへ渡す)。 */
struct AsyncLoadJob {
    /** キャッシュ挿入先のレジストリ。 */
    FAssetRegistry*           registry  = nullptr;

    /** 結果書き込み先 (worker と future で共有)。 */
    TSharedPtr<AsyncLoadState>       state;

    /** 実行するローダ。 */
    IAssetLoader*            loader    = nullptr;

    /** ロード対象のアセット ID。 */
    FAssetId                  id        = FAssetId{};

    /** ロード対象のパス (最大 259 文字 + NUL)。 */
    wchar_t                  path[260] = {};
};

/**
 * 非同期ロードを実行するワーカー関数 (FThreadPool から呼ばれる)。
 *
 * @details
 * ファイル読み込み → ローダ呼び出し → キャッシュ挿入を行い、結果かエラーを
 * state に書き込んで counter を Done() する。ジョブは TUniquePtr のスコープ抜けで自動 delete される。
 * @param user AsyncLoadJob* を指す不透明ポインタ (所有権を受け取る)。
 * @param worker 呼び出し元ワーカーのインデックス (未使用)。
 */
void AsyncLoadWorker(void* user, u32 /*worker*/) noexcept {
    TUniquePtr<AsyncLoadJob> job(static_cast<AsyncLoadJob*>(user));

    auto bytes_r = FileSystem::ReadAllBytes(job->path);
    if (bytes_r.IsErr()) {
        job->state->error = bytes_r.Error();
        job->state->has_error = true;
        job->state->counter.Done();
        return;
    }

    auto asset_r = job->loader->LoadFromBytes(job->id, bytes_r.Value());
    if (asset_r.IsErr()) {
        job->state->error = asset_r.Error();
        job->state->has_error = true;
        job->state->counter.Done();
        return;
    }

    TSharedPtr<Asset> a = Move(asset_r.Value());
    if (!a.Get()) {  // ローダが OkInit で null を返した場合の null-deref 回避
        job->state->error = ACS_ERR(Asset, 3, "loader returned null asset");
        job->state->has_error = true;
        job->state->counter.Done();
        return;
    }
    a->SetId(job->id);
    a->SetState(EAssetState::Ready);

    // キャッシュ登録
    job->registry->AsyncCacheInsert(job->id, a);

    job->state->result = Move(a);
    job->state->counter.Done();
    // job は TUniquePtr のスコープ抜けで自動 delete
}
} // namespace

void FAssetRegistry::AsyncCacheInsert(FAssetId id, TSharedPtr<Asset> a) noexcept {
    FScopedLock lk(m_Lock);
    m_Cache.Insert(id, Move(a));
}

FAssetFuture FAssetRegistry::LoadAsync(const wchar_t* path) noexcept {
    // 共有状態を作る
    auto state = MakeShared<AsyncLoadState>();
    if (!state.Get()) return FAssetFuture{};

    if (!path) {
        state->error = ACS_ERR(Asset, 11, "LoadAsync: null path");
        state->has_error = true;
        state->counter.Done();
        return FAssetFuture(Move(state));
    }

    const FAssetId id = MakeIdFromPath(path);

    // キャッシュにあれば即完了状態で返す
    {
        FScopedLock lk(m_Lock);
        const TSharedPtr<Asset>* hit = m_Cache.Find(id);
        if (hit && hit->Get()) {
            state->result = *hit;
            state->counter.Done();
            return FAssetFuture(Move(state));
        }
    }

    // ローダ選択
    IAssetLoader* loader = nullptr;
    {
        FScopedLock lk(m_Lock);
        loader = FindLoader(path);
    }
    if (!loader) {
        state->error = ACS_ERR(Asset, 12, "LoadAsync: no loader");
        state->has_error = true;
        state->counter.Done();
        return FAssetFuture(Move(state));
    }

    // ジョブを heap 確保し FThreadPool に投入
    auto job = MakeUnique<AsyncLoadJob>();
    if (!job) {
        state->error = ACS_ERR(Memory, 200, "LoadAsync: alloc");
        state->has_error = true;
        state->counter.Done();
        return FAssetFuture(Move(state));
    }
    job->registry = this;
    job->state    = state;
    job->loader   = loader;
    job->id       = id;
    // パスをコピー（最大 259 文字）
    usize i = 0;
    while (path[i] && i < 259) { job->path[i] = path[i]; ++i; }
    job->path[i] = 0;

    Task t{};
    t.fn      = &AsyncLoadWorker;
    t.user    = job.Get();
    t.counter = nullptr;     // 完了通知は state->counter 側で行う
    auto sub = FThreadPool::Submit(t);
    if (sub.IsErr()) {
        // 投入失敗時は同期的に実行
        AsyncLoadWorker(job.Release(), 0);
        return FAssetFuture(Move(state));
    }
    job.Release();   // 所有権を worker に渡した
    return FAssetFuture(Move(state));
}

TSharedPtr<Asset> FAssetRegistry::Find(FAssetId id) noexcept {
    FScopedLock lk(m_Lock);
    const TSharedPtr<Asset>* hit = m_Cache.Find(id);
    return (hit && hit->Get()) ? *hit : TSharedPtr<Asset>();
}

void FAssetRegistry::Unload(FAssetId id) noexcept {
    FScopedLock lk(m_Lock);
    m_Cache.Remove(id);
}

void FAssetRegistry::Clear() noexcept {
    FScopedLock lk(m_Lock);
    m_Cache.Clear();
}

namespace {
/** 画像ローダ実体 (プロセス寿命)。 */
ImageAssetLoader  g_image_loader;

/** WAV 音声ローダ実体。 */
WavAssetLoader    g_wav_loader;

/** MP3 音声ローダ実体。 */
Mp3AssetLoader    g_mp3_loader;

/** FLAC 音声ローダ実体。 */
FlacAssetLoader   g_flac_loader;

/** OGG Vorbis 音声ローダ実体。 */
OggAssetLoader    g_ogg_loader;

/** glTF メッシュローダ実体。 */
GltfAssetLoader   g_gltf_loader;

/** GLB メッシュローダ実体。 */
GlbAssetLoader    g_glb_loader;

/** Wavefront OBJ メッシュローダ実体。 */
ObjAssetLoader    g_obj_loader;

/** FBX メッシュローダ実体。 */
FbxAssetLoader    g_fbx_loader;

/** テキストローダ実体。 */
FTextAssetLoader   g_text_loader;

/** バイナリフォールバックローダ実体。 */
FBinaryAssetLoader g_binary_loader;

/**
 * 拡張子別名のラッパローダ。
 *
 * @details 同じローダ実体を別の拡張子でも登録できるよう、TypeId/LoadFromBytes を委譲し Extension だけ差し替える。
 */
class AliasLoader final : public IAssetLoader {
public:
    /**
     * 委譲先ローダと別名拡張子を指定して構築する。
     *
     * @param base 実処理を委譲するローダ実体。
     * @param ext このラッパが担当する拡張子。
     */
    AliasLoader(IAssetLoader* base, const char* ext) noexcept : m_Base(base), m_Ext(ext) {}

    /**
     * 委譲先のアセット型 ID を返す。
     *
     * @return base->TypeId()。
     */
    AssetType   TypeId()    const noexcept override { return m_Base->TypeId(); }

    /**
     * この別名の拡張子を返す。
     *
     * @return 構築時に渡した拡張子。
     */
    const char* Extension() const noexcept override { return m_Ext; }

    /**
     * 委譲先ローダでバイト列からアセットを生成する。
     *
     * @param id 生成アセットに割り当てる ID。
     * @param bytes ファイル全体のバイト列。
     * @return base->LoadFromBytes() の結果。
     */
    TResult<TSharedPtr<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override {
        return m_Base->LoadFromBytes(id, bytes);
    }
private:
    /** 実処理を委譲するローダ実体。 */
    IAssetLoader* m_Base;

    /** このラッパが担当する拡張子。 */
    const char*   m_Ext;
};

/** 画像ローダの .jpg 別名。 */
AliasLoader g_image_jpg { &g_image_loader, "jpg"  };

/** 画像ローダの .jpeg 別名。 */
AliasLoader g_image_jpeg{ &g_image_loader, "jpeg" };

/** 画像ローダの .bmp 別名。 */
AliasLoader g_image_bmp { &g_image_loader, "bmp"  };

/** 画像ローダの .tga 別名。 */
AliasLoader g_image_tga { &g_image_loader, "tga"  };

/** 画像ローダの .gif 別名。 */
AliasLoader g_image_gif { &g_image_loader, "gif"  };

/** 画像ローダの .hdr 別名 (HDR 画像)。 */
AliasLoader g_image_hdr { &g_image_loader, "hdr"  };

/** 画像ローダの .pic 別名。 */
AliasLoader g_image_pic { &g_image_loader, "pic"  };

/** 画像ローダの .pnm 別名。 */
AliasLoader g_image_pnm { &g_image_loader, "pnm"  };

/** 画像ローダの .ppm 別名。 */
AliasLoader g_image_ppm { &g_image_loader, "ppm"  };

/** 画像ローダの .pgm 別名。 */
AliasLoader g_image_pgm { &g_image_loader, "pgm"  };

/** 画像ローダの .psd 別名。 */
AliasLoader g_image_psd { &g_image_loader, "psd"  };

/** OGG ローダの .oga 別名 (oga は ogg の別名)。 */
AliasLoader g_ogg_oga { &g_ogg_loader, "oga" };

/** テキストローダの .json 別名。 */
AliasLoader g_text_json { &g_text_loader, "json" };

/** テキストローダの .xml 別名。 */
AliasLoader g_text_xml  { &g_text_loader, "xml"  };

/** テキストローダの .yaml 別名。 */
AliasLoader g_text_yaml { &g_text_loader, "yaml" };

/** テキストローダの .yml 別名。 */
AliasLoader g_text_yml  { &g_text_loader, "yml"  };

/** テキストローダの .toml 別名。 */
AliasLoader g_text_toml { &g_text_loader, "toml" };

/** テキストローダの .ini 別名。 */
AliasLoader g_text_ini  { &g_text_loader, "ini"  };

/** テキストローダの .csv 別名。 */
AliasLoader g_text_csv  { &g_text_loader, "csv"  };

/** テキストローダの .md 別名。 */
AliasLoader g_text_md   { &g_text_loader, "md"   };

/** テキストローダの .log 別名。 */
AliasLoader g_text_log  { &g_text_loader, "log"  };

/** テキストローダの .hlsl 別名。 */
AliasLoader g_text_hlsl { &g_text_loader, "hlsl" };

/** テキストローダの .glsl 別名。 */
AliasLoader g_text_glsl { &g_text_loader, "glsl" };

/** テキストローダの .vert 別名。 */
AliasLoader g_text_vert { &g_text_loader, "vert" };

/** テキストローダの .frag 別名。 */
AliasLoader g_text_frag { &g_text_loader, "frag" };

/** テキストローダの .lua 別名。 */
AliasLoader g_text_lua  { &g_text_loader, "lua"  };

/** テキストローダの .py 別名。 */
AliasLoader g_text_py   { &g_text_loader, "py"   };
} // namespace

// 標準ローダを 1 度に登録する
void FAssetRegistry::RegisterDefaultLoaders() noexcept {
    // 画像
    RegisterLoader(&g_image_loader);
    RegisterLoader(&g_image_jpg);  RegisterLoader(&g_image_jpeg);
    RegisterLoader(&g_image_bmp);  RegisterLoader(&g_image_tga);
    RegisterLoader(&g_image_gif);  RegisterLoader(&g_image_hdr);
    RegisterLoader(&g_image_pic);  RegisterLoader(&g_image_pnm);
    RegisterLoader(&g_image_ppm);  RegisterLoader(&g_image_pgm);
    RegisterLoader(&g_image_psd);
    // 音声
    RegisterLoader(&g_wav_loader);
    RegisterLoader(&g_mp3_loader);
    RegisterLoader(&g_flac_loader);
    RegisterLoader(&g_ogg_loader);
    RegisterLoader(&g_ogg_oga);
    // メッシュ
    RegisterLoader(&g_gltf_loader);
    RegisterLoader(&g_glb_loader);
    RegisterLoader(&g_obj_loader);
    RegisterLoader(&g_fbx_loader);
    // テキスト
    RegisterLoader(&g_text_loader);
    RegisterLoader(&g_text_json); RegisterLoader(&g_text_xml);
    RegisterLoader(&g_text_yaml); RegisterLoader(&g_text_yml);
    RegisterLoader(&g_text_toml); RegisterLoader(&g_text_ini);
    RegisterLoader(&g_text_csv);  RegisterLoader(&g_text_md);
    RegisterLoader(&g_text_log);  RegisterLoader(&g_text_hlsl);
    RegisterLoader(&g_text_glsl); RegisterLoader(&g_text_vert);
    RegisterLoader(&g_text_frag); RegisterLoader(&g_text_lua);
    RegisterLoader(&g_text_py);
    // バイナリ（フォールバック、最後）
    RegisterLoader(&g_binary_loader);
}

} // namespace acs
