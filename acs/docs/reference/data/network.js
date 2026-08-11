/* ACS リファレンス — network モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "network",
  order: 50,
  title: "network — ネットワーク基盤",
  blurb: "PC 同士を通信でつなぐための土台。<t>TCP</t>(確実に届ける接続)と <t>UDP</t>(速いが届く保証なし)の<t>ソケット</t>を、Windows の <t>WinSock</t> をくるんだ薄い<t>API</t>として提供します。マルチプレイや簡単なサーバ通信の入口です。",
  types: [
    {
      name: "CNetwork",
      kind: "クラス", header: "network/Network.h",
      summary: "ネットワーク機能全体の<b>初期化／終了</b>をまとめる入口(<t>WinSock</t> の <code>WSAStartup</code> ラッパ)。<t>ソケット</t>を使う前に一度 <code>Init()</code> を呼ぶ必要がある。",
      when: "アプリ起動時に一度 <code>CNetwork::Init()</code>。<t>CApplication</t> はネットワークを自動初期化しないので、自分で明示的に呼ぶこと。終了時に <code>Shutdown()</code>。",
      members: [
        { sig: "static TResult<void> Init()", ret: "成功 or エラー", desc: "<t>WinSock</t> を初期化する。多重に呼んでも安全(内部で<t>参照カウント</t>)。", when: "ソケット系の型を作る前に必ず。" },
        { sig: "static void Shutdown()", desc: "ネットワークを解放する。<code>Init()</code> と対で呼ぶ。" },
        { sig: "static bool IsInitialized()", ret: "初期化済みか", desc: "すでに <code>Init()</code> 済みなら true。" },
        { sig: "using FNetwork = CNetwork", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CNetwork</code> を使う。" }
      ]
    },
    {
      name: "FIpAddress",
      kind: "構造体", header: "network/IpAddress.h",
      summary: "<t>IPv4</t> アドレス(4 個の数値)とポート番号をまとめた小さな値。通信相手や自分の待ち受け先を指すのに使う。v1 では <t>IPv6</t> は未対応。",
      when: "接続先・待ち受け先を指定するあらゆる場面。文字列 \\\"127.0.0.1\\\" から作るか、定型の <code>Any()</code> / <code>Loopback()</code> を使う。",
      members: [
        { sig: "u8 octets[4]", desc: "アドレスの 4 数値。例 <code>192.168.0.1</code> なら <code>{192,168,0,1}</code>。" },
        { sig: "u16 port", desc: "ポート番号。0 は『OS 任せ/未指定』を意味する。" },
        { sig: "static FIpAddress Any()", ret: "0.0.0.0", desc: "全ネットワークインターフェイスを表す。サーバが待ち受ける時に使う。" },
        { sig: "static FIpAddress Loopback()", ret: "127.0.0.1", desc: "同じ PC 内(自分自身)を指す。ローカルテストに便利。" },
        { sig: "static FIpAddress FromString(const char* dotted)", ret: "アドレス", desc: "\\\"192.168.0.1\\\" のような文字列から作る。書式が不正なら 0.0.0.0 を返す。"}
      ]
    },
    {
      name: "FTcpConnection",
      kind: "クラス", header: "network/TcpConnection.h",
      summary: "確立済みの <t>TCP</t> 接続 1 本。<b>順序どおり・確実に</b>データを送受信できる。クライアントとして自分から繋ぐか、サーバの <t>FTcpListener</t> が受理した接続として得る。コピー不可・<t>ムーブ</t>のみで、デストラクタで自動的に切断する。",
      when: "相手と確実にデータをやり取りしたい時(チャット・ログイン・状態同期など、取りこぼしが許されない通信)。",
      members: [
        { sig: "static TResult<FTcpConnection> Connect(FIpAddress addr, u16 port)", ret: "接続 or エラー", desc: "指定アドレス/ポートへ接続を試みる。成功すれば使える <code>FTcpConnection</code> を返す。", when: "クライアント側でサーバに繋ぐ時。" },
        { sig: "isize Send(const void* data, usize size)", ret: "送れたバイト数", desc: "バッファを送信する。部分送信あり。<code>size&gt;0</code> で null、WinSock 上限超過、失敗時は -1。<code>size=0</code> は領域を参照せず 0。", when: "送る量が多い時は、戻り値が <code>size</code> 未満なら残りを再送する。" },
        { sig: "isize Recv(void* buf, usize size)", ret: "受信バイト数", desc: "データを受け取る。<code>size&gt;0</code> で null、WinSock 上限超過、失敗時は -1。<code>size&gt;0</code> で 0 なら相手が切断、<code>size=0</code> は OS を呼ばず 0。"},
        { sig: "void Close()", desc: "接続を切断する。デストラクタでも自動で呼ばれる。" },
        { sig: "TResult<void> SetNonBlocking(bool enable)", ret: "成功 or エラー", desc: "<t>ノンブロッキング</t>モードに切り替える。true で <code>Recv</code> 等が待たずに即返るようになる。", when: "ゲームループの中で他の処理を止めずに通信したい時。" },
        { sig: "bool IsValid() const", ret: "有効か", desc: "接続が生きていれば true。" },
        { sig: "FIpAddress Remote() const", ret: "相手アドレス", desc: "接続相手の <code>FIpAddress</code> を返す。" },
        { sig: "static FTcpConnection FromAccepted(uptr socket, FIpAddress remote)", desc: "内部用。<code>FTcpListener::Accept</code> が受理した<t>ソケット</t>から接続を組み立てる。通常は直接使わない。" }
      ]
    },
    {
      name: "FTcpListener",
      kind: "クラス", header: "network/TcpListener.h",
      summary: "<t>TCP</t> 接続を<b>待ち受ける側</b>(サーバ)。指定ポートで <code>Listen</code> を始め、<code>Accept()</code> でクライアントの接続を 1 本ずつ受理して <t>FTcpConnection</t> に変える。コピー不可・<t>ムーブ</t>のみ。",
      when: "自分がサーバになって、複数のクライアントからの接続を受け付けたい時。",
      members: [
        { sig: "static TResult<FTcpListener> Listen(FIpAddress addr, u16 port, u32 backlog = 16)", ret: "リスナー or エラー", desc: "指定アドレス/ポートで待ち受けを開始する。<code>addr=Any()</code> で全インターフェイス。<code>backlog</code> は保留接続の上限。", when: "サーバ起動時。" },
        { sig: "TResult<FTcpConnection> Accept()", ret: "接続 or エラー", desc: "接続を 1 本受け付ける(既定では届くまで<b>ブロック</b>)。返った <code>FTcpConnection</code> でそのクライアントと通信する。", when: "接続待ちループの中で毎回呼ぶ。" },
        { sig: "TResult<void> SetNonBlocking(bool enable)", ret: "成功 or エラー", desc: "<t>ノンブロッキング</t>に切り替える。true なら接続が無い時 <code>Accept</code> が待たずに即返る。", when: "メインループを止めずに接続を受けたい時。" },
        { sig: "TResult<FIpAddress> LocalAddress() const", ret: "待ち受け先 or エラー", desc: "OS が割り当てたローカル IPv4 アドレスとポートを返す。<code>Listen(..., 0)</code> の実ポート確認にも使える。" },
        { sig: "void Close()", desc: "待ち受けを終了する。デストラクタでも呼ばれる。" },
        { sig: "bool IsValid() const", ret: "有効か", desc: "待ち受けが生きていれば true。" }
      ]
    },
    {
      name: "FUdpSocket",
      kind: "クラス", header: "network/UdpSocket.h",
      summary: "<t>UDP</t> の<t>ソケット</t>。接続を張らずに、宛先を指定して<b>1 通ずつ</b>データ(<t>データグラム</t>)を送受信する。速いが、到達・順序は保証されない。コピー不可・<t>ムーブ</t>のみ。",
      when: "多少の取りこぼしより速さを優先したい時(対戦中の位置同期、簡易ブロードキャストなど)。確実さが要るなら <t>FTcpConnection</t> を選ぶ。",
      members: [
        { sig: "static TResult<FUdpSocket> Bind(FIpAddress addr, u16 port)", ret: "ソケット or エラー", desc: "指定アドレス/ポートに<t>バインド</t>する。受信するなら <code>port</code> を指定、送信専用なら <code>port=0</code> で OS 任せ。<code>addr=Any()</code> で全インターフェイス。", when: "UDP 通信を始める最初の一歩。" },
        { sig: "isize SendTo(FIpAddress dst_addr, u16 dst_port, const void* data, usize size)", ret: "送れたバイト数", desc: "宛先へ 1 通送る。<code>size&gt;0</code> で null、WinSock 上限超過、失敗時は -1。<code>size=0</code> も空データグラムとして送る。"},
        { sig: "isize RecvFrom(void* buf, usize size, FIpAddress& from)", ret: "受信バイト数", desc: "1 通受け取り、成功時だけ <code>from</code> に送信元を書き込む。<code>size&gt;0</code> で null、WinSock 上限超過、OS 失敗時は -1 で <code>from</code> を変えない。空データグラムも受信する。", when: "誰から届いたかを知って返信したい時。" },
        { sig: "TResult<void> SetNonBlocking(bool enable)", ret: "成功 or エラー", desc: "<t>ノンブロッキング</t>に切り替える。true なら受信が無い時 <code>RecvFrom</code> が待たずに即返る。" },
        { sig: "TResult<FIpAddress> LocalAddress() const", ret: "バインド先 or エラー", desc: "OS が割り当てたローカル IPv4 アドレスとポートを返す。<code>Bind(..., 0)</code> の実ポート確認にも使える。" },
        { sig: "void Close()", desc: "ソケットを閉じる。デストラクタでも呼ばれる。" },
        { sig: "bool IsValid() const", ret: "有効か", desc: "ソケットが生きていれば true。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "ソケット": "ネットワーク通信の出入口となる OS の口。ここを通じてデータを送受信する。",
  "TCP": "相手と接続を張り、順序どおり・確実にデータを届ける通信方式。取りこぼしが許されない用途向け。関連語 <t>UDP</t>。",
  "UDP": "接続を張らず<t>データグラム</t>を 1 通ずつ送る通信方式。速いが到達・順序は保証されない。関連語 <t>TCP</t>。",
  "WinSock": "Windows のソケット通信ライブラリ。ACS の <t>CNetwork</t> はこれを薄くくるんでいる。",
  "IPv4": "<code>192.168.0.1</code> のような 4 数値で表す主流のネットワークアドレス形式。",
  "IPv6": "IPv4 を拡張した新しいアドレス形式。ACS v1 では未対応。",
  "データグラム": "UDP で送受信する 1 通分のデータの塊。それ単体で届き、順序は保証されない。",
  "バインド": "ソケットを特定のアドレス/ポートに結び付けて、そこで送受信できるようにすること。",
  "ノンブロッキング": "受信や接続待ちの関数が、結果が無くても待たずにすぐ返る動作モード。ゲームループを止めずに通信できる。"
});
