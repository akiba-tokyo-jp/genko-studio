# PhotoCraft 全機能取込み: 調査・差分・設計確認

## 現在の結論

- 依頼元: https://github.com/storytold/photocraft/releases
- 調査対象: **v0.2.0**、コミット `ad863217386440ca968fccc9bfff65ba24e61142`。2026-10-06取得。
- Genkoの比較対象: Python安定版とC++候補版。C++基点 `f9bb7a10d0a808c838a2d6f5b65a646cbf59fd2c`。既存の別worktreeの未完修正は変更していない。
- 調査・台帳化を実施した。**新機能の製品実装・統合・公開はまだ行っていない。全機能取込み完了ではない。**
- C++20/Qt方針、既存のCPU/RAM制限、原稿・承認・保存・復旧の保護は維持する。Pythonへの差戻し、PhotoCraftの別アプリ起動だけでの取込み扱い、メニューだけの実装での完了扱いをしない。

## 1. 数の根拠

実ソースのメニューと、配布Linux CLIをネットワークなし・認証情報なし・read-only・1CPU/2GiBで実行した登録一覧を照合した。

- メニュー原データ: 627行。`edit.keyboardShortcuts`がメインメニューとWindow内に重複するため、**操作IDは626種類**。
- CLIエンジンの登録操作: **748種類**。文書なしの`enabled: false`は文書状態依存であり、未実装という意味ではない。
- メニューとエンジンの操作IDの和集合: **892種類**（両方482、メニューのみ144、エンジンのみ266）。
- 実コードのTool列挙: **42種類**。READMEの「34 tools」と一致しないため、READMEの数を検査基準にしない。
- codec列挙: **13形式**。PSD/PSB、RAW、独自`.pcraft`は別の入口であり、13という数を全入出力形式数と呼ばない。AVIFは任意featureの出力のみ。
- 横断要件14行を含む追跡台帳: **961行**。これは完成機能数・欠落機能数ではない。操作別の同等性照合と試験は未完。

`feature-ledger.json`に全IDを残した。静的に対応候補を確認した項目も「部分対応」とし、画素・パラメータ・色・深度・UI・保存の同等性を検証するまでは既存機能で充足としない。

## 2. ライセンス

ソースはMIT OR Apache-2.0。直接移植する箇所にはMITを選択し、著作権表示とライセンス本文を残せる。`ATTRIBUTION.md`の別条件も守る。

- UIフォント: SIL OFL 1.1。
- Lucideアイコン: ISC、一部はFeatherのMITも必要。
- 英語辞書: SCOWL表示条件。
- ICCプロファイル: 自作プロファイルや生成物はCC0。
- **ArtCraftの商標・ブランド素材はオープンソースではない。機能とは別なので流用しない。**

追加課金、認証変更、権限追加、クラウドAI接続は必要条件として認めない。PhotoCraftでNeural Filtersと呼ぶ機能も、同名という理由だけで外部AI APIを追加しない。実装がローカル算法かを項目別に確認する。

## 3. Genkoで既にあるものと、強化/追加が必要なもの

### 既存機能を消さず強化する群

Genkoには既に、複数ページ・コマ割り・写植/縦書き/ルビ、線画・ラスタ、フォルダー、合成・クリップ・マスク、選択範囲、各種変形、9種の色調補正、22種のラスタフィルター、非破壊の調整/効果層、テクスチャ/素材、3D参照、履歴・共同作業・制作工程/承認/MCPがある。

Python版にはPSD/PSBの入出力、CMYK出力、ICC印刷変換/色校正もある。**これらを丸ごと「Genkoにない」と数えない。** C++版に未移植なら既存移行工程の残件へ結び付ける。色校正自体ではなく、文書の色モデル・高精度保持・表示ICCなどの不足を区別する。

### 追加/拡張の主要群

1. **色・精度基盤**: チャンネル8/16/32bit、float/HDR、RGB/Gray/CMYK/Lab/Indexed/Bitmap/Duotone/Multichannel、任意チャンネル、文書プロファイル、表示ICC、ソフトプルーフ、色域警告。Genkoの8bit RGB/RGBA中心の編集・PNG素材保存を維持したままの追加だけでは高精度の意味を保持できない。
2. **補正**: 露光量、自然な彩度、白黒、カラーバランス、チャンネルミキサー、特定色域、フォトフィルター、LUT/Color Lookup、HDR Toning、Shadows/Highlights、自動補正等。既存のLevels/Curves/Hue等は多チャンネル・色深度・パラメータ差を拡張する。
3. **非破壊編集**: 埋込み/リンク式スマートオブジェクト、内容の再編集/差替え、スマートフィルター、専用マスク、フィルター再順序/再設定、スタック処理。保存した元データから再計算できることまで必要。
4. **選択・修復**: Object/Subject選択、Select and Mask、背景/マジック消去、Quick Mask、磁気/多角形選択、Color Range、Content-Aware Fill/Move/Scale、修復/パッチ/クローン、赤目。既存ラッソ等の対応と分ける。
5. **ブラシ**: PhotoCraftの42道具、混色・履歴/Art Historyブラシ、背景消去、Dodge/Burn/Sponge/Blur/Sharpen/Smudge、各種ダイナミクス、ABR互換、描画再生・差分Undo。既存筆圧・傾き・漫画ブラシは保持。
6. **変形・パス**: 自由なベジェ編集、ブール演算、シェイプ、パス上文字、Warp、Perspective/Puppet Warp、Liquify、Vanishing Point、Lens Correction、レンズ/遠近/手ぶれ等。既存のコマ形状編集とは別。
7. **フィルター**: Blur Gallery、各Blur、Render/Noise/Pixelate/Distort/Stylize/Video、Filter Gallery、Texture/Sketch/Artistic、Camera Raw等。フィルターごとに深度・色モデル・マスク・選択境界・alphaの試験が必要。
8. **RAW・高度な形式互換**: TIFF/EXR/HDR/OpenRaster/WebP/GIF/AVIF/HEIF等の能力別確認、Camera RAW現像、PSD/PSB高深度・Lab・テキスト/ベクトル/調整層/スマートオブジェクト/未知ブロック保持。Genkoの既存PSD読取はLabを明示拒否する。
9. **制作補助**: レイヤーコンプ、アートボード、ノート/計測/カウント、アクションとバッチ、データ駆動Variables/Data Sets、Contact Sheet/Picture Package/Photomerge/HDR/Align/Blend、スライス、タイムライン/フレーム/GIF/Onion Skin、校正/ヒストグラム/ナビゲーター/ワークスペース等。
10. **拡張・制御**: WASMプラグインABI v1、プリセット、JSON制御チャンネル、UI操作・文書検査・スクリーンショット。既存MCPの承認ゲートを迂回する別口を設けない。

機能群は重複する。上記の項目数を独立した機能総数に加算しない。

## 4. 実行で確認したこと

SHA256SUMSと配布tarのSHA256を照合してから、使い捨て・制限付きコンテナで上流CLIを実行した。

- `commands`: 748件のJSON登録を取得。ID重複なし。
- 16bit RGB合成文書: Exposure調整層追加成功。`document.inspect`でdepth=16、Adjustment層、Undo可能を確認。
- 32bit RGB合成文書: Exposure調整層追加成功。`document.inspect`でdepth=32を確認。
- 16bit RGB合成文書: スマートオブジェクト変換後、Gaussian Blur適用成功。inspectでSmart Object層を確認。

**上流の限定動作確認であり、Genkoの受入成功ではない。** 各フィルターの全画素、保存再読込、Undo/Redo、実GUIの保証にはまだ使わない。

## 5. 実装依存順序

- P0: 全件台帳を維持。既存のC++移行残件と新機能を分離。現行公開契約の変更に関する確認を得る。
- P1: 任意色・高精度サンプルとチャンネルを不変所有できるモデル、素材のcontent-addressed保存、拡張capability、旧readerの読取専用拒否、原稿復旧/Undo/GC、CPU/RAM予算。8bit既存契約は別経路で保つ。
- P2: 調整層・色管理・LUT・ヒストグラム。各項目をRED→GREEN、API/CLI→GUI→保存/Undoまで縦に完成。
- P3: スマートオブジェクト/スマートフィルター、パス/シェイプ、選択/修復/変形。
- P4: ブラシ/プリセット、フィルター各群、RAW/高度な入出力。共通境界を変える候補のみ全検証へ拡大。
- P5: 制作補助、アクション/バッチ、タイムライン、WASMと既存MCPの安全な接続。
- P6: 全件の不足ゼロ照合、独立レビュー、関連/全統合受入、配布前Windows検証、配布/公開。

新macOS/ブラウザ対応は既存SPECの正式対象Windows/Linuxを超える。機能の取込みとは別に追加対象を確認するまで成功/除外扱いにしない。GitHub Actionsを再有効化しない。

## 6. 共通の受入条件

- 上流のID・意味・全パラメータと対応する日本語操作を台帳へ固定。
- 適用後の実画素、alpha、領域外不変、8/16/32bit・色モデル、既存正常系を確認。
- 破壊/非破壊、マスク、スマートフィルター、保存再読込、別保存先、再編集、Undo/Redo、復旧、GCを必要条件へ結ぶ。
- GUIは実xcb/Xvfbで、実際に設定して画面/結果を観察。ボタンの存在だけで合格にしない。
- 非有限値、巨大値、巨大原稿、深い入れ子、破損形式、パストラバーサル、保存失敗、未承認/lock/read-onlyの拒否は全原稿paths/bytes不変。
- 原本/本番原稿は使わない。合成fixtureのみ。source/binary/config/fixture/scope pinsを残す。
- 同じ名前の既存操作を「対応済み」とするのは、必要パラメータ/色/深度/保存/GUIの同等性が確認できたときだけ。
- `complete`は操作登録やメニュー表示ではなく、各IDの受入証跡付き全件照合で判断。

## 7. 設計確認が必要な点

**確認結果:** 利用者は「漫画原稿へ全機能を統合し、C++版の原稿形式を拡張する」を選択した。さらに「カラーもC++では可能にします」と指定した。C++版のカラー制作をCOLOR-01へ明記。DECISIONS.jsonに実回答を保存した。以下は選択に至る設計比較であり、現在の承認待ちを意味しない。実装・受入は未完。

`PLAN.md` §5は、現行公開契約または保持機能の変更が必要になった場合を停止・確認条件にしている。PhotoCraftの高精度カラーとスマートオブジェクトを漫画ページで編集し直せる形で保存するには、現行モデル/保存の契約を拡張する必要がある。

採用案は「漫画原稿へ統合、v4のfeature/capability機構で識別、旧原稿の原本を保全、未対応readerでは編集を拒否、既存8bitの読取/画素/操作を不変に維持」。比較した代案は「写真編集用モードと専用保存形式を追加し、漫画への配置は参照素材として保持」であり、利用者はこれを選択していない。機能を黙って減らさずC++本体へ実装し、カラー制作も保持する。

## 8. ファイル

- `upstream-inventory.json`: メニュー出典、pin、release、Genko基点。
- `upstream-commands.json`: 上流CLI登録の実出力。
- `feature-ledger.json`: 全961行の取込み/照合/受入状態。
- `upstream-probe-16bit-exposure.json`, `upstream-functional-probes.json`: 上流の限定実行記録。
- `PLAN.md`: 依存順序・実装方針・停止境界。
