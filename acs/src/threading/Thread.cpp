// =============================================================================
// ACS Threading — Thread 実装 (Win32 / POSIX)
// =============================================================================
#include "threading/Thread.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"

#if ACS_PLATFORM_WINDOWS
    #include "foundation/Platform.h"

    namespace acs {

    namespace {
    struct StartCtx {
        ThreadEntry    entry;
        void*          user;
        const wchar_t* name;
    };

    DWORD WINAPI Trampoline(LPVOID arg) noexcept {
        StartCtx* ctx = static_cast<StartCtx*>(arg);
        ThreadEntry e = ctx->entry;
        void* u       = ctx->user;
        const wchar_t* name = ctx->name;
        if (name) ::SetThreadDescription(::GetCurrentThread(), name);
        ::HeapFree(::GetProcessHeap(), 0, ctx);
        e(u);
        return 0;
    }
    } // namespace

    ThreadId CurrentThreadId() noexcept {
        return ThreadId{ static_cast<u32>(::GetCurrentThreadId()) };
    }
    void SleepMs(u32 ms) noexcept { ::Sleep(static_cast<DWORD>(ms)); }
    void Yield() noexcept         { ::SwitchToThread(); }
    u32 HardwareConcurrency() noexcept {
        SYSTEM_INFO si {};
        ::GetSystemInfo(&si);
        return si.dwNumberOfProcessors == 0 ? 1 : si.dwNumberOfProcessors;
    }

    Thread::~Thread() noexcept {
        if (_handle) ::CloseHandle(_handle);
    }
    Thread::Thread(Thread&& other) noexcept : _handle(other._handle), _id(other._id) {
        other._handle = nullptr;
        other._id     = {};
    }
    Thread& Thread::operator=(Thread&& other) noexcept {
        if (this == &other) return *this;
        if (_handle) ::CloseHandle(_handle);
        _handle = other._handle; _id = other._id;
        other._handle = nullptr; other._id = {};
        return *this;
    }
    Result<Thread> Thread::Spawn(ThreadEntry entry, void* user, const ThreadConfig& cfg) noexcept {
        if (!entry) return ACS_ERR(Threading, 1, "Thread::Spawn called with null entry");
        auto* ctx = static_cast<StartCtx*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(StartCtx)));
        if (!ctx) return ACS_ERR(Memory, 1, "Thread::Spawn HeapAlloc failed");
        ctx->entry = entry; ctx->user = user; ctx->name = cfg.name;
        DWORD tid = 0;
        HANDLE h = ::CreateThread(nullptr, static_cast<SIZE_T>(cfg.stack_bytes),
                                  &Trampoline, ctx, 0, &tid);
        if (!h) {
            DWORD err = ::GetLastError();
            ::HeapFree(::GetProcessHeap(), 0, ctx);
            return ACS_ERR_OS(OS, 1, "CreateThread failed", err);
        }
        if (cfg.priority != 0) ::SetThreadPriority(h, cfg.priority);
        if (cfg.affinity != 0) ::SetThreadAffinityMask(h, static_cast<DWORD_PTR>(cfg.affinity));
        Thread t;
        t._handle = h;
        t._id     = ThreadId{ static_cast<u32>(tid) };
        return Result<Thread>(OkInit, Move(t));
    }
    void Thread::Join() noexcept {
        if (!_handle) return;
        ::WaitForSingleObject(_handle, INFINITE);
        ::CloseHandle(_handle);
        _handle = nullptr;
    }
    void Thread::Detach() noexcept {
        if (_handle) { ::CloseHandle(_handle); _handle = nullptr; }
    }

    } // namespace acs

#elif ACS_PLATFORM_POSIX
    #include <pthread.h>
    #include <unistd.h>
    #include <sched.h>
    #include <time.h>
    #include <cstdlib>
    #include <cstring>

    namespace acs {

    namespace {
    struct StartCtx {
        ThreadEntry    entry;
        void*          user;
        const wchar_t* name;     // POSIX では unused (互換のため)
    };
    void* Trampoline(void* arg) noexcept {
        StartCtx* ctx = static_cast<StartCtx*>(arg);
        ThreadEntry e = ctx->entry;
        void* u       = ctx->user;
        // POSIX 側はワイドネームを使えないので skip
        std::free(ctx);
        e(u);
        return nullptr;
    }
    } // namespace

    ThreadId CurrentThreadId() noexcept {
        // pthread_self はポインタ相当なので u64 に丸めて u32 に縮める
        // (一意性は保ちたいので下位 32bit でもまあまあ衝突しない)
        return ThreadId{ static_cast<u32>(reinterpret_cast<uintptr_t>(pthread_self())) };
    }
    void SleepMs(u32 ms) noexcept {
        timespec ts;
        ts.tv_sec  = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, nullptr);
    }
    void Yield() noexcept { sched_yield(); }
    u32 HardwareConcurrency() noexcept {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n <= 0 ? 1u : static_cast<u32>(n);
    }

    Thread::~Thread() noexcept {
        // _handle は pthread_t (cast 経由)。Detach されていなければ join しない方針。
        // Win 側と挙動を揃えるために Join/Detach されてないと OS リソースが残る。
        // ここでは何もしない (= ユーザー責任)。
    }
    Thread::Thread(Thread&& other) noexcept : _handle(other._handle), _id(other._id) {
        other._handle = nullptr; other._id = {};
    }
    Thread& Thread::operator=(Thread&& other) noexcept {
        if (this == &other) return *this;
        _handle = other._handle; _id = other._id;
        other._handle = nullptr; other._id = {};
        return *this;
    }
    Result<Thread> Thread::Spawn(ThreadEntry entry, void* user, const ThreadConfig& cfg) noexcept {
        if (!entry) return ACS_ERR(Threading, 1, "Thread::Spawn called with null entry");
        auto* ctx = static_cast<StartCtx*>(std::malloc(sizeof(StartCtx)));
        if (!ctx) return ACS_ERR(Memory, 1, "Thread::Spawn malloc failed");
        ctx->entry = entry; ctx->user = user; ctx->name = cfg.name;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if (cfg.stack_bytes > 0) {
            pthread_attr_setstacksize(&attr, cfg.stack_bytes);
        }

        pthread_t tid;
        int err = pthread_create(&tid, &attr, &Trampoline, ctx);
        pthread_attr_destroy(&attr);
        if (err != 0) {
            std::free(ctx);
            return ACS_ERR_OS(OS, 1, "pthread_create failed", static_cast<u32>(err));
        }

        // スレッド名 (Linux のみ pthread_setname_np、最大 16 文字)
#if defined(__linux__)
        if (cfg.name) {
            // wchar_t -> ASCII narrow に簡易変換 (英数字想定)
            char narrow[16] = {};
            for (int i = 0; i < 15 && cfg.name[i]; ++i) {
                wchar_t w = cfg.name[i];
                narrow[i] = (w < 128) ? static_cast<char>(w) : '?';
            }
            pthread_setname_np(tid, narrow);
        }
#endif

        Thread t;
        t._handle = reinterpret_cast<void*>(tid);
        t._id     = ThreadId{ static_cast<u32>(reinterpret_cast<uintptr_t>(tid)) };
        return Result<Thread>(OkInit, Move(t));
    }
    void Thread::Join() noexcept {
        if (!_handle) return;
        pthread_join(reinterpret_cast<pthread_t>(_handle), nullptr);
        _handle = nullptr;
    }
    void Thread::Detach() noexcept {
        if (!_handle) return;
        pthread_detach(reinterpret_cast<pthread_t>(_handle));
        _handle = nullptr;
    }

    } // namespace acs

#else
    #error "ACS Thread: unsupported platform"
#endif
