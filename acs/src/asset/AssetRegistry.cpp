// AssetRegistry 実装
#include "asset/AssetRegistry.h"
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

} // namespace acs
