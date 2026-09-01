# Wine lfsync ブランチ

このブランチは、Wine の `winehq/master` を基礎として、NT 同期オブジェクトを共有メモリ上で処理する実験的な lock-free バックエンド（以下 lfsync）と、Android・ウィンドウ管理・Vulkan 周辺の関連修正を加えたものです。

主な目的は、イベント、セマフォ、ミューテックス、および複数オブジェクト待機の主要処理を wineserver との往復なしに実行できるようにしつつ、スレッド終了、ハンドル破棄、APC、タイムアウトなど Wine が必要とする動作を維持することです。これは upstream Wine の正式機能ではなく、既定では無効です。

## 1. lock-free NT 同期バックエンド

### 基本構成

lfsync は wineserver が作成する共有 `memfd` を各 Wine プロセスへマップし、その領域に同期オブジェクト、待機スロット、トランザクション記述子、所有者情報、および寿命管理情報を配置します。

- イベント、セマフォ、ミューテックスの状態変更は、可能な限り共有メモリ上の原子的な操作で完結します。
- `WaitAny`、`WaitAll`、signal-and-wait など複数の状態を同時に扱う操作には、他スレッドが未完了の処理を補助できる MCAS（multi-word compare-and-swap）方式を使用します。
- 競合が軽い間は短時間スピンし、待機が必要になった場合は Linux futex でスリープします。
- 待機者の要約とハッシュ化された待機者リストにより、状態変更時に無関係な待機を走査しないようにしています。
- スレッド終了とミューテックス放棄を通常のトランザクションと順序付けし、終了中の所有者に対する競合を処理します。
- 記述子には世代情報を持たせ、再利用された記述子を古い参照と誤認しないようにしています。MCAS の後始末も、補助中のスレッドがいなくなるまで遅延されます。

共有領域には固定上限とレイアウトバージョンがあります。これは ABI として upstream に提供されるものではなく、このブランチ内の wineserver と ntdll が同じ実装を使用することを前提としています。

### オブジェクトの寿命管理

初期の実装で使用していたオブジェクトごとの lifetime pipe は、共有メモリ上の世代付き lease に置き換えられています。プロセスがオブジェクトを利用している間は lease を保持し、解放情報を wineserver が回収します。これにより、プロセスごとのファイルディスクリプタ消費を抑えながら、待機、所有者、および参照が残るオブジェクトの早すぎる再利用を防ぎます。

ハンドルを閉じた後も進行中の待機が参照するオブジェクトは保持されます。この考え方は、lfsync だけでなく、エクスポートされた ntsync ミューテックスの寿命修正にも適用されています。

### 有効化

ビルドした Wine を次のように起動すると lfsync が選択されます。

```sh
WINELOCKFREE_SYNC=1 ./wine program.exe
```

`WINELOCKFREE_SYNC` が未設定の場合は、利用可能なら `/dev/ntsync`、それも利用できなければ通常の wineserver ベースの経路へ進みます。

実装は環境変数の「値」ではなく「存在」を判定するため、`WINELOCKFREE_SYNC=0` でも有効になります。無効にする場合は変数を設定しないか、`unset WINELOCKFREE_SYNC` を使用してください。

## 2. Android と memfd の互換性

Android など、libc が `memfd_create()` のラッパーを公開していない環境でも、カーネルが対応していれば `SYS_memfd_create` を直接呼び出して lfsync の共有領域を作成します。どちらも利用できない場合は `ENOSYS` として扱い、既存の同期経路へフォールバックします。

この変更は lfsync のための移植性対応であり、lfsync 自体は Linux の共有メモリと futex を前提としています。lfsync の利用に `/dev/ntsync` は必要ありません。

## 3. クライアントサーフェス、X11、Vulkan

このブランチには、ウィンドウマネージャが存在しない X11 環境や Android を含むホスト環境で、クライアント描画サーフェスを安全に公開するための一連の変更も含まれます。

### ウィンドウマネージャなしの X11

X11 ルートウィンドウの `SubstructureRedirectMask` を調べ、ICCCM 対応ウィンドウマネージャが存在しない場合は managed mode を無効にします。これにより、設定されることのない `WM_STATE` を待ち続ける状況を避けます。

### サーフェスの段階的な公開

win32u と wineserver は、トップレベルおよび子ウィンドウに属するクライアントサーフェスについて、表示状態、Z オーダー、dirty/staged 状態、および公開世代を追跡します。X11 側では XComposite を用いて準備途中のサーフェスをリダイレクトし、対応する世代の描画が完了してから表示側へ反映します。

ドライバ境界の `CreateClientSurface` インターフェースも更新されており、X11 だけでなく Android、macOS、Wayland の各ドライバで整合するようになっています。

### Vulkan の present とリサイズ

各 swapchain image は acquire 時点のクライアントサーフェス世代を記録します。ホストが `VK_KHR_present_id` と `VK_KHR_present_wait` の拡張および機能を提供する場合、Wine 内部で present の完了を待ってからオフスクリーンサーフェスを公開し、前フレームや異なる世代の内容をコピーすることを防ぎます。対応していない環境では従来の経路を使用します。

クライアント領域の大きさと swapchain の大きさが一致しなくなった場合は `VK_ERROR_OUT_OF_DATE_KHR` を返し、リサイズ後の swapchain 再作成を促します。

### 非表示中の present、OpenGL の完了確認、リサイズ

非表示中に実際に present した X11 client surface は、mapped dummy parent 上の同じ native drawable を表示後も維持します。show 時の XComposite unredirect や reparent によって、完成済み EGL back buffer を失わないためです。非表示で作成されただけの通常ウィンドウにはこの維持状態を設定しません。

winex11 の EGL backend は、ウィンドウ寸法の更新を表す `GL_FLUSH_UPDATED` を受けた時に `eglQuerySurface()` で width と height を再取得します。Mesa の X11 EGL 実装はこの照会時に X geometry の変化を検出して古い drawable buffer を invalidate するため、64x64 から実寸へ拡大した直後の描画が旧 extent に clip されません。この処理は EGL 固有であり、GLX、Wine Vulkan、DXVK の内部へ直接作用するものではありません。

EGL と GLX の offscreen swap は、XDamage が対応する pixmap の更新を通知するまで待機してから client surface を完成済みとして公開します。描画完了待機の対象は swap 対象の drawable に限定し、タイムアウト時は診断を残して従来経路へ進みます。これにより、swap API の復帰だけを完成の証拠として扱っていた競合を除去します。

WGL の pixel format はプロセス内の `WND` だけでなく wineserver のウィンドウ状態として保持します。別プロセスが所有する HWND を描画する場合にも同じ形式を取得できます。また、active と cached の client surface 所有者を描画プロセス単位で追跡し、geometry-ready 通知を実際に再合成できるプロセスのキューへ送ります。終了済み描画プロセスの所有情報は自動的に破棄します。

managed resize では、`XSync()` の完了を drawable が新しい寸法になった証拠とはみなしません。トップレベルの `ConfigureNotify` で寸法遷移を確認した時点を再合成境界とし、新しく露出した端が黒く残る競合を防ぎます。

## 4. 関連する正当性修正

lfsync および Android 対応の検証中に見つかった、独立して意味のある修正も含まれています。

- `NtQueryInformationJobObject()` は wineserver が保持する job limit flags をそのまま返し、数値上限を伴わないフラグも問い合わせ間で失いません。
- `QueryFullProcessImageNameW()` が未定義の flags を受け取った場合、`ERROR_INVALID_PARAMETER` を返します。
- wineboot の prefix 更新では、更新開始時に失敗状態を記録し、必要なインストーラが成功した場合だけ新しいタイムスタンプを確定します。`disable` 指定も維持されます。
- 対象プロセスが終了した場合、実行中として登録済みの system APC をキャンセルして待機元へ通知し、永久待機を防ぎます。
- ntsync へエクスポートされたミューテックスを待機完了まで保持し、ハンドル close や所有者終了との競合で早期解放されないようにします。
- セマフォの release が失敗した場合、出力引数の値を不正に変更しません。
- crypt32 の証明書ファイル読み込みで `fdopen()` へ渡したファイルディスクリプタの所有権を正しく扱い、二重 close を防ぎます。
- crypt32 の chain テストでは、重複していた中間証明書を正しい GlobalSign ルート証明書へ置き換え、ホストの信頼ストアに依存しない期待結果を使用します。

## 5. 主な変更箇所

| 領域 | 主なファイル |
| --- | --- |
| lock-free 共通実装 | `include/wine/lockfree_sync.h`, `libs/wine/lockfree_sync.c` |
| ntdll 側の同期・待機 | `dlls/ntdll/unix/lockfree_sync_core.c`, `dlls/ntdll/unix/sync.c`, `dlls/ntdll/unix/thread.c` |
| wineserver 側の共有領域・寿命管理 | `server/lockfree_sync_core.c`, `server/inproc_sync.c`, `server/process.c`, `server/thread.c` |
| lock-free 単体テスト | `libs/wine/lockfree_sync_test.c` |
| クライアントサーフェス | `dlls/win32u/window.c`, `dlls/win32u/dce.c`, `server/window.c` |
| X11 の公開制御・OpenGL 完了待機・リサイズ | `dlls/winex11.drv/window.c`, `dlls/winex11.drv/init.c`, `dlls/winex11.drv/opengl.c`, `dlls/winex11.drv/event.c` |
| Vulkan の世代同期 | `dlls/win32u/vulkan.c`, `include/wine/vulkan_driver.h` |
| Android の互換性 | `server/inproc_sync.c`, `dlls/wineandroid.drv/opengl.c` |

## 6. 検証範囲と制限

lock-free 共通実装には、イベント、セマフォ、ミューテックス、`WaitAny`、`WaitAll`、signal-and-wait、pulse、競合、所有者終了、および lease の寿命を対象とする専用テストがあります。開発時にはこれらの単体テスト、Wine 全体のビルド、および関連する Wine テストを実行しています。

X11 の描画修正は、固定 RGB8 正解画像を持つ Win32/WGL テストアプリで、可視40状態、故障注入6種、非表示11状態を検査しています。さらに Steam のクリック、開閉、開いたままの切替、hover、4段階のリサイズを含む37遷移を検査し、固定メニュー画像を評価した66 captureで pixel mismatch、黒・白置換、外部遮蔽がすべて0であることを確認しています。

ただし、このブランチは実験段階です。固定容量の共有領域が枯渇するケース、未検証のアプリケーション固有の同期パターン、および通常とは異なるプロセス終了経路については、追加検証が必要です。性能もワークロードに依存するため、ntsync や通常の wineserver 経路に常に優越するとは限りません。

`d3dkmt` のテストには、Xvfb と llvmpipe の組み合わせで不足する OpenGL 拡張や、RADV が返す timeline semaphore 上限など、実行環境に依存する失敗があります。該当テスト本体は upstream と同一であり、これらを本ブランチの回帰とは扱っていません。ウィンドウおよび Vulkan の変更を評価する際は、使用する X server と GPU ドライバの能力を分けて確認する必要があります。

このブランチの実装と本書は AI（gpt-5.6-sol、medium）により生成・整理された実験成果であり、利用時には対象環境での再検証を前提とします。
