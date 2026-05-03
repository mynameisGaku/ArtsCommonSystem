// テキストアセット実装
#include "asset/TextAsset.h"

namespace acs {

Result<Rc<Asset>> TextAssetLoader::LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept {
    Array<char> text;
    text.Resize(bytes.Size() + 1);
    for (usize i = 0; i < bytes.Size(); ++i) text[i] = static_cast<char>(bytes[i]);
    text[bytes.Size()] = '\0';
    Rc<TextAsset> a = MakeRc<TextAsset>(Move(text));
    a->SetId(id);
    a->SetState(AssetState::Ready);
    return Result<Rc<Asset>>(OkInit, Rc<Asset>(Move(a)));
}

} // namespace acs
