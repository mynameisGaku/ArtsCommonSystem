// AssetRegistry 実装
#include "asset/AssetRegistry.h"
#include "asset/BinaryAsset.h"
#include "asset/TextAsset.h"
#include "asset/ImageAsset.h"
#include "asset/AudioAsset.h"
#include "asset/MeshAsset.h"
#include "platform/FileSystem.h"
#include "threading/ScopedLock.h"
#include "foundation/Move.h"

namespace acs {

namespace {

// wchar_t* パスから拡張子（小文字 ASCII）を取り出す。"*" は対応なしを示す。
// ascii 範囲外の拡張子は扱わない（一般的でないため）
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

bool StrEqAscii(const char* a, const char* b) noexcept {
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == 0 && *b == 0;
}

// パスから AssetId を作る（wchar_t をバイト列としてハッシュ）
AssetId MakeIdFromPath(const wchar_t* path) noexcept {
    usize len = 0;
    while (path[len]) ++len;
    return MakeAssetId(StringView(reinterpret_cast<const char*>(path), len * sizeof(wchar_t)));
}

} // namespace

void AssetRegistry::RegisterLoader(IAssetLoader* loader) noexcept {
    if (!loader) return;
    ScopedLock lk(_lock);
    _loaders.PushBack(loader);
}

IAssetLoader* AssetRegistry::FindLoader(const wchar_t* path) noexcept {
    char ext[32]{};
    ExtractExtensionAscii(path, ext, sizeof(ext));
    IAssetLoader* fallback = nullptr;
    for (usize i = 0; i < _loaders.Size(); ++i) {
        const char* e = _loaders[i]->Extension();
        if (StrEqAscii(e, "*")) fallback = _loaders[i];
        if (StrEqAscii(e, ext)) return _loaders[i];
    }
    return fallback;  // 拡張子マッチなければ "*" のフォールバックを返す
}

Result<Rc<Asset>> AssetRegistry::Load(const wchar_t* path) noexcept {
    if (!path) return ACS_ERR(Asset, 1, "AssetRegistry::Load: null path");

    AssetId id = MakeIdFromPath(path);

    // キャッシュにあればそのまま返す
    {
        ScopedLock lk(_lock);
        Rc<Asset>* hit = _cache.Find(id);
        if (hit && hit->Get()) return Result<Rc<Asset>>(OkInit, *hit);
    }

    // 拡張子から適切なローダを選ぶ
    IAssetLoader* loader = nullptr;
    {
        ScopedLock lk(_lock);
        loader = FindLoader(path);
    }
    if (!loader) return ACS_ERR(Asset, 2, "no loader for this asset path");

    // ファイルを読み込む
    auto bytes_r = FileSystem::ReadAllBytes(path);
    if (bytes_r.IsErr()) return Err<Rc<Asset>>(bytes_r.Error());

    // ローダ呼び出し
    auto asset_r = loader->LoadFromBytes(id, bytes_r.Value());
    if (asset_r.IsErr()) return Err<Rc<Asset>>(asset_r.Error());

    Rc<Asset> a = Move(asset_r.Value());
    a->SetId(id);
    a->SetState(AssetState::Ready);

    // キャッシュに登録
    {
        ScopedLock lk(_lock);
        _cache.Insert(id, a);
    }
    return Result<Rc<Asset>>(OkInit, Move(a));
}

Rc<Asset> AssetRegistry::Find(AssetId id) noexcept {
    ScopedLock lk(_lock);
    Rc<Asset>* hit = _cache.Find(id);
    return (hit && hit->Get()) ? *hit : Rc<Asset>();
}

void AssetRegistry::Unload(AssetId id) noexcept {
    ScopedLock lk(_lock);
    _cache.Remove(id);
}

void AssetRegistry::Clear() noexcept {
    ScopedLock lk(_lock);
    _cache.Clear();
}

namespace {
// 標準ローダ実体（プロセス寿命）
ImageAssetLoader  g_image_loader;
WavAssetLoader    g_wav_loader;
Mp3AssetLoader    g_mp3_loader;
FlacAssetLoader   g_flac_loader;
OggAssetLoader    g_ogg_loader;
GltfAssetLoader   g_gltf_loader;
GlbAssetLoader    g_glb_loader;
ObjAssetLoader    g_obj_loader;
FbxAssetLoader    g_fbx_loader;
TextAssetLoader   g_text_loader;
BinaryAssetLoader g_binary_loader;

// 拡張子別名のラッパ（同じ実体を別の拡張子で登録できるようにする）
class AliasLoader final : public IAssetLoader {
public:
    AliasLoader(IAssetLoader* base, const char* ext) noexcept : _base(base), _ext(ext) {}
    AssetType   TypeId()    const noexcept override { return _base->TypeId(); }
    const char* Extension() const noexcept override { return _ext; }
    Result<Rc<Asset>> LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept override {
        return _base->LoadFromBytes(id, bytes);
    }
private:
    IAssetLoader* _base;
    const char*   _ext;
};

// 画像 (jpg/jpeg/bmp/tga/gif/hdr/pic/pnm/ppm/pgm/psd)
AliasLoader g_image_jpg { &g_image_loader, "jpg"  };
AliasLoader g_image_jpeg{ &g_image_loader, "jpeg" };
AliasLoader g_image_bmp { &g_image_loader, "bmp"  };
AliasLoader g_image_tga { &g_image_loader, "tga"  };
AliasLoader g_image_gif { &g_image_loader, "gif"  };
AliasLoader g_image_hdr { &g_image_loader, "hdr"  };
AliasLoader g_image_pic { &g_image_loader, "pic"  };
AliasLoader g_image_pnm { &g_image_loader, "pnm"  };
AliasLoader g_image_ppm { &g_image_loader, "ppm"  };
AliasLoader g_image_pgm { &g_image_loader, "pgm"  };
AliasLoader g_image_psd { &g_image_loader, "psd"  };

// 音声 (oga は ogg の別名)
AliasLoader g_ogg_oga { &g_ogg_loader, "oga" };

// テキスト (各拡張子)
AliasLoader g_text_json { &g_text_loader, "json" };
AliasLoader g_text_xml  { &g_text_loader, "xml"  };
AliasLoader g_text_yaml { &g_text_loader, "yaml" };
AliasLoader g_text_yml  { &g_text_loader, "yml"  };
AliasLoader g_text_toml { &g_text_loader, "toml" };
AliasLoader g_text_ini  { &g_text_loader, "ini"  };
AliasLoader g_text_csv  { &g_text_loader, "csv"  };
AliasLoader g_text_md   { &g_text_loader, "md"   };
AliasLoader g_text_log  { &g_text_loader, "log"  };
AliasLoader g_text_hlsl { &g_text_loader, "hlsl" };
AliasLoader g_text_glsl { &g_text_loader, "glsl" };
AliasLoader g_text_vert { &g_text_loader, "vert" };
AliasLoader g_text_frag { &g_text_loader, "frag" };
AliasLoader g_text_lua  { &g_text_loader, "lua"  };
AliasLoader g_text_py   { &g_text_loader, "py"   };
} // namespace

// 標準ローダを 1 度に登録する
void AssetRegistry::RegisterDefaultLoaders() noexcept {
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
