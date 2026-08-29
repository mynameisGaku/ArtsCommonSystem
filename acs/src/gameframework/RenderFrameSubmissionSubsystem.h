// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_GAMEFRAMEWORK_RENDER_FRAME_SUBMISSION_SUBSYSTEM_H
#define ACS_GAMEFRAMEWORK_RENDER_FRAME_SUBMISSION_SUBSYSTEM_H


#include "render/RendererFrameEndResult.h"
#include "subsystem/Subsystem.h"

namespace acs::game {

/**
 * 1つのWorldで記録した描画命令へ、実際のGPU提出結果を返す共有境界。
 * 描画を所有するCGameが発行し、現在のシーンにある1つの描画実装が購読する。
 */
class CRenderFrameSubmissionSubsystem final : public ASubsystem {
public:
    /** 提出結果を受け取る非所有listener関数。 */
    using FListener = void (*)(
        void* listener,
        const FRendererFrameEndResult& result) noexcept;

    ACS_GAME_SUBSYSTEM_KIND(CRenderFrameSubmissionSubsystem)

    /**
     * 現在のWorldで提出結果を受け取るlistenerを登録する。
     * null、または別listenerが登録済みならfalseを返し、既存登録を保つ。
     */
    bool TryBind(void* listener, FListener callback) noexcept;

    /** 指定listenerが登録中の場合だけ購読を解除する。 */
    void Unbind(void* listener) noexcept;

    /** SubmitとPresentを終えた結果を、登録中のlistenerへ一度通知する。 */
    void Publish(const FRendererFrameEndResult& result) noexcept;

    /** World終了時に非所有listenerを破棄前へ持ち越さない。 */
    void OnDeinitialize() noexcept override;

private:
    /** 現在のWorldで結果を受け取る非所有listener。 */
    void* m_Listener = nullptr;

    /** listenerへ結果を渡す関数。 */
    FListener m_Callback = nullptr;
};

} // namespace acs::game

namespace acs {

/** GameFrameworkの描画提出通知をトップレベルから参照する入口。 */
using game::CRenderFrameSubmissionSubsystem;

} // namespace acs

#endif // ACS_GAMEFRAMEWORK_RENDER_FRAME_SUBMISSION_SUBSYSTEM_H
