# ポーズ集の見直しの記録（SPEC FIGURE-02）

人形のポーズ集（`native/src/core/data/figure_poses.json`、genko.poses@1）の全ポーズを、一覧の絵（正面・斜め・横、床の線、接地の点、確かめに落ちたものは赤い枠）で見た記録。絵は `GENKO_POSE_SHEETS=<フォルダ> test_figure_poses` が書く（`genko studio figure-poses --sheet DIR` でも書ける）。新しいポーズは、見て ok か直した（fixed）と書いてから入れる。

- 見た人 `agent` はこの版を作ったエージェント（人の目ではない。利用者の目で見直したら、その日付と名前の行を足す）。
- fixed: 絵か確かめを見て直したもの（直した点を書く）。ok: そのまま通したもの。
- 自動の確かめ（関節の範囲・接地・寝た体の面・重心・体の通り抜け・反転・描ける・小さな絵が同じバイト）は全ポーズが通る（test_figure_poses）。
- バッチ A（立つ〜日常の動作）は群 G3、バッチ B（見せポーズ〜物を持つ）と、あぐらの 4 つの作り直しは群 G4、バッチ C（スポーツ〜二人のポーズ）は群 G5。

| 分類 | 数 | 群 |
|---|---|---|
| 立つ（stand） | 46 | G3 |
| ファッション（fashion） | 27 | G3 |
| 歩く（walk） | 11 | G3 |
| 走る（run） | 11 | G3 |
| 椅子に座る（sit_chair） | 33 | G3 |
| 床に座る（sit_floor） | 30 | G3 |
| ひざまずく・しゃがむ（kneel_squat） | 26 | G3 |
| 日常の動作（everyday） | 26 | G3 |
| 見せポーズ（グラビア・ピンナップ風）（pinup） | 78 | G4 |
| ベッド・ソファでくつろぐ（relax_bed） | 33 | G4 |
| 寝る・横になる（lie） | 20 | G4 |
| 上半身の仕草（upper_body） | 35 | G4 |
| 振り向く・回る（turn） | 11 | G4 |
| 指さす（point） | 8 | G4 |
| 感情（emotion） | 26 | G4 |
| 話す（talk） | 13 | G4 |
| 物を持つ（hands_props） | 27 | G4 |
| スポーツ（sports_action） | 38 | G5 |
| アクション（打つ・蹴る・構える・剣・投げる・倒れる）（action） | 30 | G5 |
| ダンス（dance） | 20 | G5 |
| ヒーロー・決めポーズ（heroic） | 20 | G5 |
| 仕事と暮らし（work_life） | 26 | G5 |
| カップル（二人）（couple） | 30 | G5 |
| 二人のポーズ（two_people） | 30 | G5 |
| 計 | 655 | |


## 立つ（stand）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `stand/neutral` | まっすぐ立つ | 2026-11-15 | agent | ok |  |
| `stand/attention` | 気をつけ | 2026-11-15 | agent | ok |  |
| `stand/at_ease` | 休めの姿勢 | 2026-11-15 | agent | fixed | 腕が大きくねじれていた → 片手で片手を後ろで受ける形にした |
| `stand/hands_on_hips` | 腰に手を当てて立つ | 2026-11-15 | agent | ok |  |
| `stand/arms_crossed` | 腕を組んで立つ | 2026-11-15 | agent | fixed | 前腕が胸に入り、肘が体の前へ寄っていた → 肘を横へ、前腕を胸の前へ離した |
| `stand/hands_in_pockets` | 両手をポケットに入れて立つ | 2026-11-15 | agent | ok |  |
| `stand/one_hand_pocket` | 片手をポケットに入れて立つ | 2026-11-15 | agent | ok |  |
| `stand/contrapposto` | 片足に重心をかけて立つ | 2026-11-15 | agent | ok |  |
| `stand/wall_lean_back` | 壁に背中でもたれる | 2026-11-15 | agent | ok |  |
| `stand/wall_lean_shoulder` | 壁に肩でもたれる | 2026-11-15 | agent | ok |  |
| `stand/look_back` | 振り返る | 2026-11-15 | agent | ok |  |
| `stand/look_up` | 見上げる | 2026-11-15 | agent | ok |  |
| `stand/look_down` | うつむく | 2026-11-15 | agent | ok |  |
| `stand/shield_eyes` | 手をかざして遠くを見る | 2026-11-15 | agent | ok |  |
| `stand/wave` | 手を振る | 2026-11-15 | agent | ok |  |
| `stand/wave_both` | 両手を振る | 2026-11-15 | agent | ok |  |
| `stand/point_forward` | 前を指さす | 2026-11-15 | agent | ok |  |
| `stand/point_side` | 横を指さす | 2026-11-15 | agent | ok |  |
| `stand/point_up` | 上を指さす | 2026-11-15 | agent | ok |  |
| `stand/raise_hand` | 手を挙げる | 2026-11-15 | agent | ok |  |
| `stand/thumbs_up` | 親指を立てる | 2026-11-15 | agent | ok |  |
| `stand/salute` | 敬礼 | 2026-11-15 | agent | ok |  |
| `stand/stretch_overhead` | 両手を上げて伸びをする | 2026-11-15 | agent | fixed | 腕のねじれが範囲の外 → 角度で直接（手のひらを向かい合わせ） |
| `stand/stretch_side` | 体の横を伸ばす | 2026-11-15 | agent | ok |  |
| `stand/v_arms` | 両手を上げてVの字 | 2026-11-15 | agent | ok |  |
| `stand/arms_out` | 両腕を広げる | 2026-11-15 | agent | ok |  |
| `stand/surrender` | 両手を上げて手のひらを見せる | 2026-11-15 | agent | ok |  |
| `stand/palms_forward` | 手のひらを前に向ける | 2026-11-15 | agent | ok |  |
| `stand/hands_clasped_front` | 前で手を組む | 2026-11-15 | agent | fixed | 両手が重なって通り抜けていた → 片手をもう片手の上に |
| `stand/hands_clasped_behind` | 後ろで手を組む | 2026-11-15 | agent | fixed | 同上 |
| `stand/hands_behind_head` | 頭の後ろで手を組む | 2026-11-15 | agent | fixed | 手首が前腕に入っていた → 手を頭の後ろで少し離した |
| `stand/palms_together` | 胸の前で手を合わせる | 2026-11-15 | agent | fixed | 手首どうしがめり込んでいた → 手首を離して指先で合わせた |
| `stand/hand_on_heart` | 胸に手を当てる | 2026-11-15 | agent | fixed | 指が外を向いていた → 胸の中ほどへ向けた |
| `stand/hand_on_chin` | あごに手を当てて考える | 2026-11-15 | agent | ok |  |
| `stand/scratch_head` | 頭をかく | 2026-11-15 | agent | ok |  |
| `stand/power_stance` | 堂々と仁王立ち | 2026-11-15 | agent | ok |  |
| `stand/fists` | 拳を握って構える | 2026-11-15 | agent | ok |  |
| `stand/shrug` | 肩をすくめる | 2026-11-15 | agent | ok |  |
| `stand/tiptoe` | つま先立ちで背伸び | 2026-11-15 | agent | fixed | 重心がつま先より前で倒れる → 足の母指球を体の下へ |
| `stand/ankles_crossed` | 足を交差させて立つ | 2026-11-15 | agent | fixed | 交差した足がもう片方のすねに入っていた → 前へずらした |
| `stand/hands_on_knees` | 膝に手をついて前かがみ | 2026-11-15 | agent | ok |  |
| `stand/bow` | お辞儀 | 2026-11-15 | agent | fixed | 手が太ももの中に入っていた → 太ももの前に当てた |
| `stand/profile` | 横向きに立つ | 2026-11-15 | agent | ok |  |
| `stand/three_quarter` | 斜め向きに立つ | 2026-11-15 | agent | ok |  |
| `stand/back_view` | 後ろ姿 | 2026-11-15 | agent | fixed | 左右対称の印を外した（腰の向き 180° は反転と数が違う） |
| `stand/back_glance` | 後ろ姿で振り向く | 2026-11-15 | agent | ok |  |

## ファッション（fashion）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `fashion/runway_walk` | ランウェイを歩く | 2026-11-15 | agent | ok |  |
| `fashion/hand_in_hair` | 髪に手をやる | 2026-11-15 | agent | ok |  |
| `fashion/both_hands_hair` | 両手で髪をかき上げる | 2026-11-15 | agent | ok |  |
| `fashion/legs_crossed` | 足を交差して片手を腰に | 2026-11-15 | agent | ok |  |
| `fashion/coat_open` | 上着の前を開く | 2026-11-15 | agent | ok |  |
| `fashion/over_shoulder` | 肩越しに振り向く | 2026-11-15 | agent | fixed | 向きを変えた体に腕の向きが付いてこなかった → 体の向きに合わせた |
| `fashion/hip_pop` | 腰を突き出して立つ | 2026-11-15 | agent | ok |  |
| `fashion/back_pockets` | 後ろのポケットに手を入れる | 2026-11-15 | agent | ok |  |
| `fashion/toe_point` | つま先を立てて片足を前に | 2026-11-15 | agent | ok |  |
| `fashion/hat_brim` | 帽子のつばに手をかける | 2026-11-15 | agent | ok |  |
| `fashion/collar_touch` | 襟元に手をやる | 2026-11-15 | agent | ok |  |
| `fashion/sunglasses` | サングラスに手をかける | 2026-11-15 | agent | ok |  |
| `fashion/shoulder_bag` | 肩のバッグの紐を持つ | 2026-11-15 | agent | fixed | 肘の曲がりが範囲の外 → 紐を少し下で持つ |
| `fashion/jacket_over_shoulder` | 上着を肩にかける | 2026-11-15 | agent | fixed | 肘の曲がりが範囲の外 → 肩の上で軽く曲げる形に |
| `fashion/wall_foot_up` | 壁にもたれて片足をかける | 2026-11-15 | agent | ok |  |
| `fashion/arms_crossed_hip` | 腕を組んで腰を落とす | 2026-11-15 | agent | ok |  |
| `fashion/hand_on_wall` | 壁に片手をつく | 2026-11-15 | agent | ok |  |
| `fashion/chin_up_profile` | 横顔であごを上げる | 2026-11-15 | agent | ok |  |
| `fashion/arch_back` | 腰に手を当てて背を反らす | 2026-11-15 | agent | ok |  |
| `fashion/hands_on_thighs` | 腿に手を置いて前かがみ | 2026-11-15 | agent | ok |  |
| `fashion/twirl` | くるりと回る | 2026-11-15 | agent | fixed | 軸足の上に重心が無かった → 軸足を体の下へ |
| `fashion/steps_sit` | 階段に腰かける | 2026-11-15 | agent | fixed | 膝を開くと股関節の範囲の外 → 骨盤を後ろへ傾け、座面を高く |
| `fashion/stool_perch` | スツールに浅く腰かける | 2026-11-15 | agent | ok |  |
| `fashion/squat_chin` | しゃがんで頬杖をつく | 2026-11-15 | agent | fixed | 膝を開くと股関節の範囲の外 → 骨盤を後ろへ傾け、少し浅く |
| `fashion/lean_railing` | 手すりに腕を乗せて寄りかかる | 2026-11-15 | agent | ok |  |
| `fashion/walk_turn` | 歩きながら振り向く | 2026-11-15 | agent | ok |  |
| `fashion/kneel_pose` | 片膝をついてポーズ | 2026-11-15 | agent | fixed | 後ろの脚が床に付いていなかった → 膝をつき、つま先を立てた |

## 歩く（walk）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `walk/step` | 歩く（踏み出し） | 2026-11-15 | agent | ok |  |
| `walk/passing` | 歩く（片足立ち） | 2026-11-15 | agent | fixed | 振り出した足が床に付いていた → 膝を曲げて上げた |
| `walk/brisk` | 早足で歩く | 2026-11-15 | agent | ok |  |
| `walk/stroll_pockets` | ポケットに手を入れてぶらぶら歩く | 2026-11-15 | agent | ok |  |
| `walk/phone` | スマホを見ながら歩く | 2026-11-15 | agent | ok |  |
| `walk/look_back` | 歩きながら後ろを見る | 2026-11-15 | agent | ok |  |
| `walk/sneak` | 忍び足で歩く | 2026-11-15 | agent | ok |  |
| `walk/hands_behind` | 後ろで手を組んで歩く | 2026-11-15 | agent | ok |  |
| `walk/carry_bag` | かばんを提げて歩く | 2026-11-15 | agent | ok |  |
| `walk/march` | 行進する | 2026-11-15 | agent | ok |  |
| `walk/stairs_up` | 階段を上る | 2026-11-15 | agent | ok |  |

## 走る（run）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `run/jog_stance` | ジョギング（着地） | 2026-11-15 | agent | ok |  |
| `run/jog_flight` | ジョギング（宙に浮く） | 2026-11-15 | agent | ok |  |
| `run/stance` | 走る（着地） | 2026-11-15 | agent | ok |  |
| `run/flight` | 走る（宙に浮く） | 2026-11-15 | agent | ok |  |
| `run/sprint_drive` | 全力疾走（踏み込み） | 2026-11-15 | agent | ok |  |
| `run/sprint_flight` | 全力疾走（宙に浮く） | 2026-11-15 | agent | ok |  |
| `run/look_back` | 走りながら振り返る | 2026-11-15 | agent | ok |  |
| `run/reach_out` | 手を伸ばして駆け寄る | 2026-11-15 | agent | ok |  |
| `run/brake` | 急に止まる | 2026-11-15 | agent | ok |  |
| `run/skip` | スキップする | 2026-11-15 | agent | ok |  |
| `run/girly` | 手を横に振って走る | 2026-11-15 | agent | ok |  |

## 椅子に座る（sit_chair）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `sit_chair/upright` | 背筋を伸ばして座る | 2026-11-15 | agent | ok |  |
| `sit_chair/relaxed` | 楽に座る | 2026-11-15 | agent | ok |  |
| `sit_chair/hands_on_lap` | 膝の上で手を重ねる | 2026-11-15 | agent | ok |  |
| `sit_chair/knees_together` | 膝をそろえて内股で座る | 2026-11-15 | agent | fixed | 女の体で前腕どうしが重なった → 手を膝の上に |
| `sit_chair/wide_knees` | 足を開いて座る | 2026-11-15 | agent | ok |  |
| `sit_chair/legs_crossed` | 脚を組んで座る | 2026-11-15 | agent | ok |  |
| `sit_chair/ankles_crossed` | 足首を交差させて座る | 2026-11-15 | agent | fixed | 足どうしが重なっていた → 前後にずらした |
| `sit_chair/lean_back` | 背もたれにもたれる | 2026-11-15 | agent | ok |  |
| `sit_chair/lean_forward` | 膝に肘をついて前かがみ | 2026-11-15 | agent | ok |  |
| `sit_chair/chin_in_hand` | 頬杖をつく | 2026-11-15 | agent | ok |  |
| `sit_chair/arms_crossed` | 腕を組んで座る | 2026-11-15 | agent | ok |  |
| `sit_chair/slouch` | だらっと座る | 2026-11-15 | agent | ok |  |
| `sit_chair/arm_over_back` | 背もたれに腕をかけて振り向く | 2026-11-15 | agent | ok |  |
| `sit_chair/straddle_backwards` | 椅子に逆向きにまたがる | 2026-11-15 | agent | ok |  |
| `sit_chair/reading` | 本を読む | 2026-11-15 | agent | ok |  |
| `sit_chair/phone` | スマホを操作する | 2026-11-15 | agent | ok |  |
| `sit_chair/phone_call` | 電話で話す | 2026-11-15 | agent | ok |  |
| `sit_chair/drinking` | カップで飲む | 2026-11-15 | agent | ok |  |
| `sit_chair/typing` | キーボードを打つ | 2026-11-15 | agent | ok |  |
| `sit_chair/writing` | 机で書きものをする | 2026-11-15 | agent | ok |  |
| `sit_chair/thinking` | 手をあごに当てて考える | 2026-11-15 | agent | ok |  |
| `sit_chair/stretch` | 座ったまま伸びをする | 2026-11-15 | agent | ok |  |
| `sit_chair/look_back` | 座ったまま振り返る | 2026-11-15 | agent | fixed | 肩が内へねじれすぎ → 座面の横に手をつく形に |
| `sit_chair/edge` | 椅子に浅く腰かける | 2026-11-15 | agent | ok |  |
| `sit_chair/hands_behind_head` | 頭の後ろで手を組んでもたれる | 2026-11-15 | agent | ok |  |
| `sit_chair/tired` | 疲れてうなだれる | 2026-11-15 | agent | ok |  |
| `sit_chair/talking` | 身ぶりを交えて話す | 2026-11-15 | agent | ok |  |
| `sit_chair/hug_knee` | 片膝を抱えて座る | 2026-11-15 | agent | ok |  |
| `sit_chair/legs_out` | 脚を前に投げ出して座る | 2026-11-15 | agent | fixed | 交差した足が重なっていた → 揃えて伸ばす形に |
| `sit_chair/bar_stool` | 高いスツールに座る | 2026-11-15 | agent | ok |  |
| `sit_chair/eating` | 食事をする | 2026-11-15 | agent | ok |  |
| `sit_chair/slumped_desk` | 机に突っ伏す | 2026-11-15 | agent | ok |  |
| `sit_chair/figure_four` | 片足首を膝に乗せて座る | 2026-11-15 | agent | fixed | G3 で外した（股関節の外向きのねじれの範囲）。関節座標系の測り方では範囲の中に入った → 右の足首を左の膝の上へ（足首は膝より少し高く）。G4 で戻した |

## 床に座る（sit_floor）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `sit_floor/seiza` | 正座 | 2026-11-15 | agent | fixed | 足首が床の下へ入っていた → すねを床に沿わせ、足の甲は床のすぐ上（足首が甲より太い形のため 0.13 u ほど浮く） |
| `sit_floor/seiza_bow` | 正座でお辞儀 | 2026-11-15 | agent | fixed | 手が床に届かない → 深く礼をした |
| `sit_floor/seiza_tea` | 正座で湯のみを持つ | 2026-11-15 | agent | ok |  |
| `sit_floor/agura` | あぐら | 2026-11-15 | agent | fixed | （G3）股関節の範囲の中で膝を床まで下ろせない → 膝を少し上げたあぐら（足は反対の膝の下）。（G4）膝が上がり、すねが X 字に交わっていた（股関節のねじれの測り方を関節座標系に直した）→ 太ももを外へ低く、すねを前で重ね、足の外側を下に |
| `sit_floor/agura_chin` | あぐらで頬杖 | 2026-11-15 | agent | fixed | （G3）前へ倒すと座面が浮いた → 背中だけを丸めた。（G4）同上 |
| `sit_floor/meditation` | 瞑想する | 2026-11-15 | agent | fixed | 同上 |
| `sit_floor/agura_book` | あぐらで本を読む | 2026-11-15 | agent | fixed | 同上 |
| `sit_floor/hug_knees` | 体育座り | 2026-11-15 | agent | fixed | 足が近すぎて膝が胸より上に → 足を前へ |
| `sit_floor/face_on_knees` | 膝に顔をうずめる | 2026-11-15 | agent | fixed | 太ももが胴に入っていた → 背中を少し起こした |
| `sit_floor/legs_out` | 脚を伸ばして座る | 2026-11-15 | agent | fixed | 床に手が届かない（腕より肩が高い）→ 手を腿の上に |
| `sit_floor/lean_back_hands` | 後ろに手をついて座る | 2026-11-15 | agent | fixed | 手が床に届かない → 上体をもっと後ろへ倒した |
| `sit_floor/look_up_sky` | 後ろに手をついて空を見上げる | 2026-11-15 | agent | fixed | 同上 |
| `sit_floor/one_knee_up` | 片膝を立てて座る | 2026-11-15 | agent | fixed | 同上 → 伸ばした脚の腿に手 |
| `sit_floor/forward_fold` | 前屈ストレッチ | 2026-11-15 | agent | fixed | 手が足に入っていた → すねを持つ形に |
| `sit_floor/legs_apart` | 開脚して座る | 2026-11-15 | agent | fixed | 開きすぎ（股関節の範囲の外）、手が届かない → 少し閉じ、前へ倒した |
| `sit_floor/one_leg_tucked` | 片脚を曲げて座る | 2026-11-15 | agent | ok |  |
| `sit_floor/knees_up_arms` | 立てた膝に腕を乗せる | 2026-11-15 | agent | ok |  |
| `sit_floor/game` | ゲームをする | 2026-11-15 | agent | ok |  |
| `sit_floor/phone` | 床でスマホを見る | 2026-11-15 | agent | ok |  |
| `sit_floor/stretch_up` | 正座で伸びをする | 2026-11-15 | agent | ok |  |
| `sit_floor/look_back` | 床に座って振り返る | 2026-11-15 | agent | fixed | 脚の組み方が床の下へ入った → 膝を立てて座り、後ろの手は床の近く |
| `sit_floor/hug_one_knee` | 片膝を抱えて座る | 2026-11-15 | agent | ok |  |
| `sit_floor/knees_up_wide` | 膝を立てて足を開いて座る | 2026-11-15 | agent | fixed | 同上、膝を開きすぎていた |
| `sit_floor/legs_out_crossed_arms` | 脚を伸ばして腕を組む | 2026-11-15 | agent | ok |  |
| `sit_floor/seiza_hands_lap` | 正座で手を重ねる | 2026-11-15 | agent | ok |  |
| `sit_floor/lean_hands_knees_up` | 膝を立てて後ろに手をつく | 2026-11-15 | agent | fixed | 同上 |
| `sit_floor/knees_chin` | 膝の上で頬杖をつく | 2026-11-15 | agent | ok |  |
| `sit_floor/seiza_look_up` | 正座で見上げる | 2026-11-15 | agent | ok |  |
| `sit_floor/seiza_fingers` | 正座で手をつく | 2026-11-15 | agent | fixed | 同上 |
| `sit_floor/seiza_head_down` | 正座でうつむく | 2026-11-15 | agent | ok |  |

## ひざまずく・しゃがむ（kneel_squat）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `kneel_squat/one_knee` | 片膝立ち | 2026-11-15 | agent | ok |  |
| `kneel_squat/propose` | 片膝をついて手を差し出す | 2026-11-15 | agent | ok |  |
| `kneel_squat/both_knees` | 膝立ち | 2026-11-15 | agent | ok |  |
| `kneel_squat/kiza` | つま先を立てて正座 | 2026-11-15 | agent | fixed | つま先が床に入っていた → 太ももを立て、かかとを上げた |
| `kneel_squat/pray` | ひざまずいて祈る | 2026-11-15 | agent | ok |  |
| `kneel_squat/kneel_look_down` | 膝立ちでうなだれる | 2026-11-15 | agent | ok |  |
| `kneel_squat/kneel_victory` | 膝立ちで両手を上げる | 2026-11-15 | agent | ok |  |
| `kneel_squat/squat_toes` | つま先立ちでしゃがむ | 2026-11-15 | agent | fixed | 重心がつま先より前 → 足を前へ |
| `kneel_squat/squat_flat` | かかとをつけてしゃがむ | 2026-11-15 | agent | fixed | 足首の曲がりが範囲の外 → 少し浅く |
| `kneel_squat/squat_hug` | しゃがんで膝を抱える | 2026-11-15 | agent | ok |  |
| `kneel_squat/squat_sad` | しゃがんで顔を伏せる | 2026-11-15 | agent | ok |  |
| `kneel_squat/squat_elbows` | しゃがんで膝に肘をのせる | 2026-11-15 | agent | ok |  |
| `kneel_squat/squat_reach` | しゃがんで手を伸ばす | 2026-11-15 | agent | ok |  |
| `kneel_squat/squat_inspect` | しゃがんで地面を指さす | 2026-11-15 | agent | ok |  |
| `kneel_squat/squat_photo` | しゃがんでカメラを構える | 2026-11-15 | agent | ok |  |
| `kneel_squat/crouch_ready` | 低く身構える | 2026-11-15 | agent | ok |  |
| `kneel_squat/crouch_hide` | 身をかがめて隠れる | 2026-11-15 | agent | ok |  |
| `kneel_squat/pick_up` | 片膝をついて物を拾う | 2026-11-15 | agent | fixed | 手が床に届かない → 深く前へかがんだ |
| `kneel_squat/hero_landing` | 片膝をついて着地する | 2026-11-15 | agent | fixed | 手が床に届かない（肩が高い）→ 片膝の着地だけに（名前も） |
| `kneel_squat/all_fours` | 四つん這い | 2026-11-15 | agent | ok |  |
| `kneel_squat/dogeza` | 土下座 | 2026-11-15 | agent | fixed | 同上 → 足の甲を床に、膝を前に |
| `kneel_squat/lunge_stretch` | 片膝をついて股関節を伸ばす | 2026-11-15 | agent | ok |  |
| `kneel_squat/kneel_look_back` | 片膝立ちで振り返る | 2026-11-15 | agent | ok |  |
| `kneel_squat/deep_squat_arms` | 腕を前に伸ばしてしゃがむ | 2026-11-15 | agent | fixed | 同上 |
| `kneel_squat/half_squat` | 中腰になる | 2026-11-15 | agent | ok |  |
| `kneel_squat/kneel_hands_thighs` | 膝立ちで腿に手を置く | 2026-11-15 | agent | ok |  |

## 日常の動作（everyday）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `everyday/read_standing` | 立って本を読む | 2026-11-15 | agent | ok |  |
| `everyday/texting` | スマホを両手で操作する | 2026-11-15 | agent | ok |  |
| `everyday/phone_call` | 立って電話をかける | 2026-11-15 | agent | ok |  |
| `everyday/drink` | 立ってカップで飲む | 2026-11-15 | agent | ok |  |
| `everyday/carry_box` | 箱を抱えて運ぶ | 2026-11-15 | agent | ok |  |
| `everyday/carry_bag` | かばんを肩にかける | 2026-11-15 | agent | ok |  |
| `everyday/cooking` | 鍋をかき混ぜる | 2026-11-15 | agent | ok |  |
| `everyday/wipe_table` | テーブルを拭く | 2026-11-15 | agent | ok |  |
| `everyday/sweep` | ほうきで掃く | 2026-11-15 | agent | ok |  |
| `everyday/check_watch` | 腕時計を見る | 2026-11-15 | agent | ok |  |
| `everyday/tie_shoes` | 靴ひもを結ぶ | 2026-11-15 | agent | fixed | 後ろの脚が大きく後ろへ伸びていた → 膝を体の下へ |
| `everyday/brush_teeth` | 歯をみがく | 2026-11-15 | agent | ok |  |
| `everyday/selfie` | 自撮りをする | 2026-11-15 | agent | ok |  |
| `everyday/umbrella` | 傘をさす | 2026-11-15 | agent | ok |  |
| `everyday/push` | 重い物を押す | 2026-11-15 | agent | ok |  |
| `everyday/pull` | 綱を引く | 2026-11-15 | agent | ok |  |
| `everyday/yawn` | あくびをする | 2026-11-15 | agent | ok |  |
| `everyday/lift_box` | 床から荷物を持ち上げる | 2026-11-15 | agent | fixed | 足首の曲がりが範囲の外 → 少し浅く |
| `everyday/hang_laundry` | 洗濯物を干す | 2026-11-15 | agent | ok |  |
| `everyday/open_door` | ドアを開ける | 2026-11-15 | agent | ok |  |
| `everyday/write_board` | 黒板に書く | 2026-11-15 | agent | ok |  |
| `everyday/call_out` | 手を挙げて呼び止める | 2026-11-15 | agent | ok |  |
| `everyday/mirror_hair` | 鏡を見て髪を整える | 2026-11-15 | agent | ok |  |
| `everyday/search_bag` | かばんの中を探す | 2026-11-15 | agent | ok |  |
| `everyday/eat_onigiri` | 立っておにぎりを食べる | 2026-11-15 | agent | ok |  |
| `everyday/waiting` | 人を待つ | 2026-11-15 | agent | ok |  |

## 見せポーズ（グラビア・ピンナップ風）（pinup）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `pinup/hip_hand_hair` | 腰を突き出して片手を髪に | 2026-11-15 | agent | ok |  |
| `pinup/hand_behind_head` | 片手を頭の後ろにやって立つ | 2026-11-15 | agent | fixed | 腕が後ろへ上がりすぎ → 手を頭の横寄りに |
| `pinup/arch_hands_behind_head` | 頭の後ろで手を組んで背を反らす | 2026-11-15 | agent | fixed | 背を反らすと重心がかかとより後ろ → 足を後ろへ |
| `pinup/forearms_on_head` | 頭の上で腕を組む | 2026-11-15 | agent | fixed | 腕が上がりすぎ → 前腕を頭の上に軽くのせた |
| `pinup/arch_low_back` | 腰に両手を当てて胸を張る | 2026-11-15 | agent | fixed | 同上 |
| `pinup/back_glance_hips` | 後ろ姿で腰に手を当てて振り返る | 2026-11-15 | agent | ok |  |
| `pinup/back_glance_hair` | 後ろ姿で髪に手をやって振り返る | 2026-11-15 | agent | fixed | 同上 |
| `pinup/back_view_arms_up` | 後ろ姿で両腕を上げる | 2026-11-15 | agent | ok |  |
| `pinup/profile_chest_out` | 横向きで胸を張る | 2026-11-15 | agent | fixed | 足の向きを体の向きに重ねて足し、脚のねじれが範囲の外 → 足は体に対して少しだけ外へ |
| `pinup/wall_hands_behind` | 壁に背中をつけて後ろで手を組む | 2026-11-15 | agent | ok |  |
| `pinup/wall_arm_up` | 壁にもたれて片腕を上げる | 2026-11-15 | agent | ok |  |
| `pinup/wall_shoulder_twist` | 壁に肩をつけて腰をひねる | 2026-11-15 | agent | ok |  |
| `pinup/wall_hands_look_back` | 壁に両手をついて振り返る | 2026-11-15 | agent | ok |  |
| `pinup/knee_in` | 片膝を内に寄せて立つ | 2026-11-15 | agent | ok |  |
| `pinup/tiptoe_reach` | つま先立ちで両手を上に伸ばす | 2026-11-15 | agent | fixed | 重心がつま先より前 → 母指球を体の下へ |
| `pinup/hair_flip_knee` | 髪をかき上げて片膝を曲げる | 2026-11-15 | agent | ok |  |
| `pinup/lean_hand_thigh` | 片手を腿に置いて身を乗り出す | 2026-11-15 | agent | ok |  |
| `pinup/lip_finger_twist` | 唇に指を当てて腰をひねる | 2026-11-15 | agent | ok |  |
| `pinup/knees_hands_look_up` | 膝に手をついて見上げる | 2026-11-15 | agent | ok |  |
| `pinup/flamingo` | 片足を後ろに曲げて立つ | 2026-11-15 | agent | ok |  |
| `pinup/s_curve` | S字に体をくねらせて立つ | 2026-11-15 | agent | ok |  |
| `pinup/self_hug_twist` | 自分を抱くように腕を回してひねる | 2026-11-15 | agent | ok |  |
| `pinup/hair_up_both` | 両手で髪を持ち上げる | 2026-11-15 | agent | ok |  |
| `pinup/peace_tilt` | 首をかしげてピース | 2026-11-15 | agent | ok |  |
| `pinup/lean_cheek` | 前かがみで頬に手を当てる | 2026-11-15 | agent | ok |  |
| `pinup/hand_chest_hip` | 片手を胸に、片手を腰に | 2026-11-15 | agent | ok |  |
| `pinup/lean_hands_behind` | 後ろで手を組んで前かがみ | 2026-11-15 | agent | ok |  |
| `pinup/bend_touch_shins` | 前屈してすねに手を添える | 2026-11-15 | agent | fixed | 片手を足首へ伸ばすと腕のねじれが範囲の外 → 両手をすねに添える形に（名前も） |
| `pinup/rail_elbows_back` | 手すりに両肘をかけて背をあずける | 2026-11-15 | agent | ok |  |
| `pinup/shy_look_down` | 肩をすくめて見下ろす | 2026-11-15 | agent | ok |  |
| `pinup/step_wave` | 一歩踏み出して手を振る | 2026-11-15 | agent | ok |  |
| `pinup/crossed_ankles_lean` | 足首を交差させて片手を頭に | 2026-11-15 | agent | fixed | 交差した足どうしが重なった → 前の足を外へずらした |
| `pinup/jacket_shoulder_glance` | 肩越しに見つめて片手を腰に | 2026-11-15 | agent | fixed | 同上 |
| `pinup/arms_wide_sky` | 両腕を広げて空を仰ぐ | 2026-11-15 | agent | ok |  |
| `pinup/hip_hands_front_lean` | 腰に手を当てて前に身を乗り出す | 2026-11-15 | agent | ok |  |
| `pinup/kneel_arch` | 膝立ちで背を反らす | 2026-11-15 | agent | ok |  |
| `pinup/kneel_hair` | 膝立ちで両手を髪に | 2026-11-15 | agent | fixed | 腰の傾きで片膝が浮いた → 腰をまっすぐに |
| `pinup/heels_hands_thighs` | かかとに座って腿に手を置く | 2026-11-15 | agent | fixed | 膝を開きすぎて足首が範囲の外 → 少し閉じた（名前も） |
| `pinup/kneel_lean_in` | かかとに座って身を乗り出す | 2026-11-15 | agent | fixed | 片手を床につくと手が床に届かず足首も範囲の外 → かかとに座って身を乗り出す形に（名前も） |
| `pinup/kneel_lean_back` | 膝をついて後ろに手をつく | 2026-11-15 | agent | fixed | 後ろへ傾けると太ももも回り足首が範囲の外 → 太ももの角度を傾きの分だけ足した |
| `pinup/all_fours_look_back` | 四つん這いで振り返る | 2026-11-15 | agent | fixed | 手のひらの向きを決めると腕のねじれが範囲の外 → 手は床に自然に |
| `pinup/kneel_thighs_lean` | 膝立ちで腿に手を置いて身を乗り出す | 2026-11-15 | agent | fixed | 前へ傾けすぎて重心が膝より前 → 浅く |
| `pinup/kneel_side_bend` | 膝立ちで体を横に反らす | 2026-11-15 | agent | ok |  |
| `pinup/kneel_arms_crossed` | 膝立ちで腕を組む | 2026-11-15 | agent | ok |  |
| `pinup/kneel_hand_head` | 膝立ちで片手を頭の後ろに | 2026-11-15 | agent | fixed | 同上 |
| `pinup/kneel_one_knee_hair` | 片膝立ちで髪に手をやる | 2026-11-15 | agent | ok |  |
| `pinup/squat_peace` | しゃがんでピース | 2026-11-15 | agent | ok |  |
| `pinup/lean_back_knee_up` | 後ろに手をついて片膝を立てる | 2026-11-15 | agent | ok |  |
| `pinup/hug_knee_look` | 片膝を抱えて振り向く | 2026-11-15 | agent | ok |  |
| `pinup/legs_out_ankles_crossed` | 脚を伸ばして足首を組み、後ろに手をつく | 2026-11-15 | agent | ok |  |
| `pinup/reach_forward` | 脚を伸ばして前に手を伸ばす | 2026-11-15 | agent | ok |  |
| `pinup/elbows_recline` | 両肘をついて寝そべる | 2026-11-15 | agent | ok |  |
| `pinup/beach_recline` | 脚を伸ばして後ろに寄りかかる（浜辺） | 2026-11-15 | agent | ok |  |
| `pinup/knees_up_hair` | 膝を立てて髪に手をやる | 2026-11-15 | agent | fixed | 同上 |
| `pinup/seiza_hands_thighs` | 正座で背筋を伸ばして手を腿に | 2026-11-15 | agent | ok |  |
| `pinup/one_knee_chin` | 片膝に頬をのせて座る | 2026-11-15 | agent | ok |  |
| `pinup/sideways_back_arm` | 椅子に横向きに座って背もたれに腕をかける | 2026-11-15 | agent | ok |  |
| `pinup/legs_crossed_lean` | 脚を組んで後ろにもたれる | 2026-11-15 | agent | ok |  |
| `pinup/lean_forward_hands_knees` | 膝に手を置いて前かがみに座る | 2026-11-15 | agent | ok |  |
| `pinup/edge_legs_out` | 浅く腰かけて脚を前に伸ばす | 2026-11-15 | agent | ok |  |
| `pinup/one_knee_up` | 椅子の上で片膝を立てる | 2026-11-15 | agent | ok |  |
| `pinup/hands_head_legs_crossed` | 頭の後ろで手を組んで脚を組む | 2026-11-15 | agent | ok |  |
| `pinup/stool_rung` | スツールに浅く腰かけ片足を桟にかける | 2026-11-15 | agent | ok |  |
| `pinup/stool_hands_seat` | スツールで両手を座面について身を乗り出す | 2026-11-15 | agent | ok |  |
| `pinup/hair_touch_seated` | 座って髪に手をやる | 2026-11-15 | agent | ok |  |
| `pinup/lean_on_knee_cheek` | 膝に肘をついて頬杖 | 2026-11-15 | agent | ok |  |
| `pinup/chair_straddle_arms` | 椅子に逆向きにまたがって背もたれに腕を重ねる | 2026-11-15 | agent | ok |  |
| `pinup/legs_crossed_hand_knee` | 脚を組んで膝に両手を重ねる | 2026-11-15 | agent | ok |  |
| `pinup/back_knees_crossed` | 仰向けで膝を立てて脚を組む | 2026-11-15 | agent | fixed | 頭の後ろの手で体が浮いた → 手は体の横の床に |
| `pinup/back_one_leg_up` | 仰向けで片脚をまっすぐ上げる | 2026-11-15 | agent | ok |  |
| `pinup/front_knee_bent_look` | うつ伏せで片膝を曲げてこちらを見る | 2026-11-15 | agent | fixed | 同上 |
| `pinup/front_feet_crossed_up` | うつ伏せで肘をつき、足を交差させて上げる | 2026-11-15 | agent | fixed | 頬杖では肘が床に届かず重心が支えの外 → 肘をついて手を床に（名前も） |
| `pinup/look_up_hands_chest` | 胸の前で手を組んで見上げる | 2026-11-15 | agent | ok |  |
| `pinup/skirt_pinch` | 裾をつまんで広げる | 2026-11-15 | agent | ok |  |
| `pinup/waist_twist` | 両手を腰に当ててひねる | 2026-11-15 | agent | ok |  |
| `pinup/open_chest_stretch` | 両手を後ろに伸ばして胸を開く | 2026-11-15 | agent | ok |  |
| `pinup/kneel_heart` | 膝立ちで両手でハート | 2026-11-15 | agent | ok |  |
| `pinup/back_glance_kneel` | 後ろ姿で膝立ちして振り返る | 2026-11-15 | agent | ok |  |

## ベッド・ソファでくつろぐ（relax_bed）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `relax_bed/edge_sit` | ベッドの端に腰かける | 2026-11-15 | agent | ok |  |
| `relax_bed/edge_stretch` | ベッドの端で伸びをする | 2026-11-15 | agent | ok |  |
| `relax_bed/edge_lean_back` | ベッドの端で後ろに手をつく | 2026-11-15 | agent | ok |  |
| `relax_bed/edge_phone` | ベッドの端でスマホを見る | 2026-11-15 | agent | ok |  |
| `relax_bed/edge_slump` | ベッドに腰かけてうなだれる | 2026-11-15 | agent | ok |  |
| `relax_bed/edge_hug_knee` | ベッドの端で片膝を抱える | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_deep` | ソファに深く座る | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_legs_crossed` | ソファで脚を組んでくつろぐ | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_arms_back` | ソファの背に両腕をかける | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_hug_cushion` | クッションを抱えて座る | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_doze` | ソファでうたた寝する | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_slouch_phone` | ソファにずり落ちてスマホ | 2026-11-15 | agent | ok |  |
| `relax_bed/headboard_lean` | ヘッドボードにもたれて座る | 2026-11-15 | agent | fixed | お腹の上で手と前腕が重なった → 手を左右に離した |
| `relax_bed/headboard_book` | ヘッドボードにもたれて本を読む | 2026-11-15 | agent | ok |  |
| `relax_bed/wake_stretch` | 起き上がって伸びをする | 2026-11-15 | agent | ok |  |
| `relax_bed/hug_knees_tilt` | ベッドの上で膝を抱えて首をかしげる | 2026-11-15 | agent | ok |  |
| `relax_bed/seiza_pillow` | ベッドの上で枕を抱えて正座 | 2026-11-15 | agent | ok |  |
| `relax_bed/agura_laptop` | ベッドの上であぐらをかいてパソコン | 2026-11-15 | agent | ok |  |
| `relax_bed/back_phone_both` | ベッドで両手でスマホを掲げる | 2026-11-15 | agent | ok |  |
| `relax_bed/back_arms_up_knee` | 両手を頭の上に投げ出して寝転ぶ | 2026-11-15 | agent | ok |  |
| `relax_bed/front_feet_up_phone` | うつ伏せで足を上げてスマホ | 2026-11-15 | agent | ok |  |
| `relax_bed/front_sprawl` | うつ伏せで大の字 | 2026-11-15 | agent | fixed | 同上 |
| `relax_bed/side_pillow_hug` | 枕を抱えて横向きに寝る | 2026-11-15 | agent | fixed | 横向きの腕の向き（lie と同じ直し） |
| `relax_bed/side_phone` | 横向きに寝てスマホを見る | 2026-11-15 | agent | fixed | 同上 |
| `relax_bed/side_curl_blanket` | 毛布にくるまって丸くなる | 2026-11-15 | agent | fixed | 手を胸に寄せると上腕が胸に入った → 腕を前へ。男の体で上のすねが下の太ももに入った → 上の膝を高く |
| `relax_bed/knees_up_belly` | 仰向けで膝を立ててお腹に手を置く | 2026-11-15 | agent | fixed | 「片膝を横に倒す」は足首が範囲の外 → 両膝を立ててお腹に手を置く形に（名前も） |
| `relax_bed/sofa_feet_up` | ソファで肘掛けに足をのせて寝そべる | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_armrest_head` | ソファの肘掛けに頭をのせて寝そべる | 2026-11-15 | agent | ok |  |
| `relax_bed/back_hands_chest` | 胸の上で手を組んで眠る | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_side_lie` | ソファで横向きに寝そべる | 2026-11-15 | agent | fixed | 横向きの腕の向き（lie と同じ直し） |
| `relax_bed/edge_look_back` | ベッドの端に腰かけて振り向く | 2026-11-15 | agent | fixed | 「猫のように伸びをする」は膝立ちから胴を水平にすると手が床に届かない → ベッドの端に腰かけて振り向く形に（名前も） |
| `relax_bed/edge_hands_knees` | ベッドの端で膝に手を置いて座る | 2026-11-15 | agent | ok |  |
| `relax_bed/sofa_phone` | ソファに座ってスマホを見る | 2026-11-15 | agent | ok |  |

## 寝る・横になる（lie）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `lie/back_straight` | 仰向けに寝る | 2026-11-15 | agent | fixed | 腕を床に置くと肘が体の下へ入り体が浮いた → 肘を横へ |
| `lie/back_knees_up` | 仰向けで膝を立てる | 2026-11-15 | agent | fixed | 同上 |
| `lie/back_one_knee` | 仰向けで片膝を立てる | 2026-11-15 | agent | ok |  |
| `lie/back_hands_behind_head` | 頭の後ろで手を組んで寝そべる | 2026-11-15 | agent | fixed | 頭の後ろの手が頭の下に入り体が浮いた → 手を頭の両わきへ |
| `lie/back_ankles_crossed` | 仰向けで足首を組む | 2026-11-15 | agent | fixed | 同上 |
| `lie/back_sleeping` | 仰向けで眠る | 2026-11-15 | agent | ok |  |
| `lie/back_arm_over_eyes` | 腕で目をおおって寝転ぶ | 2026-11-15 | agent | ok |  |
| `lie/back_phone_up` | 寝転んでスマホを見上げる | 2026-11-15 | agent | ok |  |
| `lie/back_book_up` | 寝転んで本を掲げて読む | 2026-11-15 | agent | ok |  |
| `lie/back_stretch` | 寝たまま伸びをする | 2026-11-15 | agent | fixed | 頭の上へ伸ばす腕が肩の範囲の外（内へ）→ 腕の角度で直接 |
| `lie/back_reach_up` | 仰向けで片手を上に伸ばす | 2026-11-15 | agent | fixed | 上へ伸ばす腕が頭の方へ上がりすぎ → 天井へまっすぐ |
| `lie/back_feet_up` | 仰向けで膝を曲げて脚を上げる | 2026-11-15 | agent | ok |  |
| `lie/front_elbows_look` | うつ伏せで肘をついて前を見る | 2026-11-15 | agent | fixed | あごを手にのせると肘が床に届かず重心が支えの外 → 肘をついて前を見る形に（名前も） |
| `lie/front_reading` | うつ伏せで本を読む | 2026-11-15 | agent | ok |  |
| `lie/side_sleep` | 横向きに寝る | 2026-11-15 | agent | fixed | 横向きの腕を床に置く手が肩のねじれの範囲の外 → 下の腕は前へ、上の腕は胸の前に角度で直接。接地に side |
| `lie/side_fetal` | 体を丸めて横向きに寝る | 2026-11-15 | agent | fixed | 同上 |
| `lie/side_head_on_arm` | 腕枕で横向きに寝る | 2026-11-15 | agent | fixed | 同上（下の腕は頭の下） |
| `lie/side_straight` | 横向きにまっすぐ寝る | 2026-11-15 | agent | fixed | 同上 |
| `lie/side_hug_knees` | 横向きで膝を抱える | 2026-11-15 | agent | fixed | 上の脚のすねが下の太ももに入った → 上の膝を高く |
| `lie/back_hand_chest` | 仰向けで片手を胸に置く | 2026-11-15 | agent | ok |  |

## 上半身の仕草（upper_body）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `upper_body/hand_on_cheek` | 頬に手を当てる | 2026-11-15 | agent | ok |  |
| `upper_body/both_cheeks` | 両手で頬をはさむ | 2026-11-15 | agent | ok |  |
| `upper_body/shh` | 口に指を当てて「しーっ」 | 2026-11-15 | agent | ok |  |
| `upper_body/cover_mouth` | 手で口をおおう | 2026-11-15 | agent | ok |  |
| `upper_body/peace_eye` | 目元でピース | 2026-11-15 | agent | ok |  |
| `upper_body/chin_on_fist` | 拳にあごをのせる | 2026-11-15 | agent | ok |  |
| `upper_body/finger_temple` | こめかみに指を当てる | 2026-11-15 | agent | ok |  |
| `upper_body/listen` | 耳に手を当てて聞く | 2026-11-15 | agent | ok |  |
| `upper_body/shout_hands` | 口に手を添えて叫ぶ | 2026-11-15 | agent | ok |  |
| `upper_body/facepalm` | 顔に手を当ててあきれる | 2026-11-15 | agent | ok |  |
| `upper_body/nape_scratch` | 首の後ろに手をやる | 2026-11-15 | agent | fixed | 腕が後ろへ上がりすぎて肩の範囲の外 → 肘を前へ上げた |
| `upper_body/hair_tuck` | 髪を耳にかける | 2026-11-15 | agent | ok |  |
| `upper_body/rub_eyes` | 目をこする | 2026-11-15 | agent | ok |  |
| `upper_body/wipe_tear` | 涙をぬぐう | 2026-11-15 | agent | ok |  |
| `upper_body/glasses` | 眼鏡を押し上げる | 2026-11-15 | agent | ok |  |
| `upper_body/hand_forehead` | 額に手の甲を当てる | 2026-11-15 | agent | ok |  |
| `upper_body/hands_on_head` | 頭を抱える | 2026-11-15 | agent | ok |  |
| `upper_body/cover_ears` | 耳をふさぐ | 2026-11-15 | agent | ok |  |
| `upper_body/stroke_chin` | あごをなでる | 2026-11-15 | agent | ok |  |
| `upper_body/pinch_nose` | 目頭を押さえる | 2026-11-15 | agent | ok |  |
| `upper_body/blow_kiss` | 投げキッス | 2026-11-15 | agent | ok |  |
| `upper_body/heart_hands` | 両手でハート | 2026-11-15 | agent | fixed | 頭の傾きで左右対称でなかった → 頭は前後だけ |
| `upper_body/wave_by_face` | 顔の横で小さく手を振る | 2026-11-15 | agent | ok |  |
| `upper_body/no_no` | 顔の前で手を振って断る | 2026-11-15 | agent | ok |  |
| `upper_body/peek_fingers` | 指の間からのぞく | 2026-11-15 | agent | ok |  |
| `upper_body/fist_up` | 拳を握って気合い | 2026-11-15 | agent | ok |  |
| `upper_body/thumb_self` | 親指で自分を指す | 2026-11-15 | agent | ok |  |
| `upper_body/point_nose` | 自分の鼻を指さす | 2026-11-15 | agent | ok |  |
| `upper_body/ok_sign` | OKサイン | 2026-11-15 | agent | ok |  |
| `upper_body/hug_self` | 自分の体を抱く | 2026-11-15 | agent | ok |  |
| `upper_body/neck_stretch` | 首を横に伸ばす | 2026-11-15 | agent | fixed | 首筋に手を当てると肘の曲がりが範囲の外 → 手を頭の上に置いて首を横に伸ばす形に（名前も） |
| `upper_body/collar_grab` | 胸元をつかむ | 2026-11-15 | agent | ok |  |
| `upper_body/fidget` | 胸の前で指をもじもじ | 2026-11-15 | agent | fixed | 袖口をつまむ形は上腕が胸に入った → 胸の前で指をもじもじする形に（名前も） |
| `upper_body/clap` | 手をたたく | 2026-11-15 | agent | fixed | 女の体で前腕と手が重なった → 手を体から離した |
| `upper_body/beckon` | 手招きする | 2026-11-15 | agent | ok |  |

## 振り向く・回る（turn）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `turn/glance_back` | 肩越しにちらりと振り向く | 2026-11-15 | agent | ok |  |
| `turn/half_turn` | 体ごと半分振り向く | 2026-11-15 | agent | ok |  |
| `turn/called` | 呼ばれて振り向く | 2026-11-15 | agent | ok |  |
| `turn/spin` | 片足でくるりと回る | 2026-11-15 | agent | fixed | 重心が軸足の外 → 軸足を体の下へ |
| `turn/step_around` | 足を引いて向きを変える | 2026-11-15 | agent | ok |  |
| `turn/look_down_back` | 振り返って足もとを見る | 2026-11-15 | agent | ok |  |
| `turn/twist_stretch` | 腰をひねる | 2026-11-15 | agent | ok |  |
| `turn/back_wave` | 背を向けたまま手を振る | 2026-11-15 | agent | ok |  |
| `turn/pivot_point` | 振り向きざまに指さす | 2026-11-15 | agent | ok |  |
| `turn/hip_look_back` | 腰に手を当てて振り返る | 2026-11-15 | agent | fixed | 反対の肩に手をやると上腕が胸に入った → 腰に手を当てて振り返る形に（名前も） |
| `turn/surprised_turn` | 驚いて振り向く | 2026-11-15 | agent | ok |  |

## 指さす（point）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `point/at_viewer` | こちらを指さす | 2026-11-15 | agent | ok |  |
| `point/down` | 足もとを指さす | 2026-11-15 | agent | ok |  |
| `point/behind_thumb` | 親指で後ろを指す | 2026-11-15 | agent | fixed | 肘の曲がりが範囲の外 → 手を肩から離した |
| `point/both_hands` | 両手で同じ方を指さす | 2026-11-15 | agent | fixed | 右腕が体の前で交差して肩がねじれた → 右手も前へ向けた |
| `point/accuse` | 身を乗り出して指を突きつける | 2026-11-15 | agent | ok |  |
| `point/up_diagonal` | 斜め上を指さす | 2026-11-15 | agent | ok |  |
| `point/self` | 自分を指さす | 2026-11-15 | agent | ok |  |
| `point/board` | 指し棒のように説明する | 2026-11-15 | agent | ok |  |

## 感情（emotion）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `emotion/joy_cheer` | 両手を上げて喜ぶ | 2026-11-15 | agent | ok |  |
| `emotion/joy_jump` | 跳び上がって喜ぶ | 2026-11-15 | agent | ok |  |
| `emotion/joy_fist_pump` | ガッツポーズ | 2026-11-15 | agent | ok |  |
| `emotion/joy_clasp` | 胸の前で手を組んで喜ぶ | 2026-11-15 | agent | ok |  |
| `emotion/anger_stomp` | 拳を握って怒る | 2026-11-15 | agent | ok |  |
| `emotion/anger_shake_fist` | 拳を振り上げて怒る | 2026-11-15 | agent | ok |  |
| `emotion/anger_arms_crossed` | 腕を組んでむっとする | 2026-11-15 | agent | ok |  |
| `emotion/anger_point` | 怒って指さす | 2026-11-15 | agent | ok |  |
| `emotion/sad_slump` | しょんぼりうなだれる | 2026-11-15 | agent | ok |  |
| `emotion/sad_cry` | 顔をおおって泣く | 2026-11-15 | agent | ok |  |
| `emotion/sad_hug` | 自分を抱えて沈む | 2026-11-15 | agent | ok |  |
| `emotion/sad_wipe` | 目をぬぐって泣く | 2026-11-15 | agent | ok |  |
| `emotion/surprise_hands_up` | 両手を上げて驚く | 2026-11-15 | agent | ok |  |
| `emotion/surprise_cheeks` | 頬を両手ではさんで驚く | 2026-11-15 | agent | ok |  |
| `emotion/surprise_step_back` | のけぞって驚く | 2026-11-15 | agent | ok |  |
| `emotion/embarrassed_cheeks` | 頬に手を当てて照れる | 2026-11-15 | agent | ok |  |
| `emotion/embarrassed_hide` | 顔を隠して照れる | 2026-11-15 | agent | ok |  |
| `emotion/embarrassed_fidget` | もじもじする | 2026-11-15 | agent | fixed | 内股でかかとを上げると重心が支えの外 → かかとを下ろした |
| `emotion/fear_cower` | 腕をかざしておびえる | 2026-11-15 | agent | ok |  |
| `emotion/fear_shield_head` | 頭をかばってうずくまる | 2026-11-15 | agent | fixed | 頭の上で前腕と手が重なり、重心が前 → 手を頭の両わきに、足を前へ |
| `emotion/fear_back_away` | 後ずさりする | 2026-11-15 | agent | ok |  |
| `emotion/think_chin` | あごに手を当てて首をかしげる | 2026-11-15 | agent | ok |  |
| `emotion/think_arms_crossed` | 腕を組んで考え込む | 2026-11-15 | agent | ok |  |
| `emotion/think_idea` | ひらめく | 2026-11-15 | agent | ok |  |
| `emotion/relief` | ほっと胸をなでおろす | 2026-11-15 | agent | ok |  |
| `emotion/tired` | ぐったり疲れる | 2026-11-15 | agent | ok |  |

## 話す（talk）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `talk/explain` | 手を広げて説明する | 2026-11-15 | agent | ok |  |
| `talk/both_hands` | 両手で身ぶりをして話す | 2026-11-15 | agent | ok |  |
| `talk/count_fingers` | 指を折って数える | 2026-11-15 | agent | ok |  |
| `talk/present` | 手で示して紹介する | 2026-11-15 | agent | ok |  |
| `talk/whisper` | 口元に手を当ててささやく | 2026-11-15 | agent | ok |  |
| `talk/argue` | 指を立てて言い返す | 2026-11-15 | agent | ok |  |
| `talk/listen_nod` | 手を前で組んで聞く | 2026-11-15 | agent | ok |  |
| `talk/raise_to_speak` | 手を挙げて発言する | 2026-11-15 | agent | ok |  |
| `talk/shrug_talk` | 肩をすくめて言う | 2026-11-15 | agent | ok |  |
| `talk/chat_lean` | 身を乗り出して話しかける | 2026-11-15 | agent | ok |  |
| `talk/phone_talk_walk` | 電話で話しながら身ぶり | 2026-11-15 | agent | fixed | 耳もとの手の肘の曲がりが範囲の外 → 手を耳から少し離した |
| `talk/hands_on_hips_scold` | 腰に手を当てて叱る | 2026-11-15 | agent | ok |  |
| `talk/greet_bow` | 会釈して話しかける | 2026-11-15 | agent | ok |  |

## 物を持つ（hands_props）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `hands_props/cup_both_hands` | 両手でカップを包む | 2026-11-15 | agent | ok |  |
| `hands_props/cup_offer` | カップを差し出す | 2026-11-15 | agent | ok |  |
| `hands_props/cup_blow` | カップに息を吹きかける | 2026-11-15 | agent | ok |  |
| `hands_props/phone_look` | 片手でスマホを見る | 2026-11-15 | agent | ok |  |
| `hands_props/phone_photo` | スマホを構えて写真を撮る | 2026-11-15 | agent | ok |  |
| `hands_props/phone_show` | スマホの画面を見せる | 2026-11-15 | agent | ok |  |
| `hands_props/paper_read` | 紙を両手で持って読む | 2026-11-15 | agent | ok |  |
| `hands_props/paper_hand_over` | 紙を両手で差し出す | 2026-11-15 | agent | ok |  |
| `hands_props/paper_hold_up` | 紙を掲げて見せる | 2026-11-15 | agent | ok |  |
| `hands_props/bag_front` | 鞄を前で両手に提げる | 2026-11-15 | agent | ok |  |
| `hands_props/bag_behind` | 鞄を後ろ手に持つ | 2026-11-15 | agent | ok |  |
| `hands_props/bag_over_shoulder` | 肩にかけた鞄の紐を握る | 2026-11-15 | agent | fixed | 肩の上で紐を持つと肘の曲がりが範囲の外 → 胸の前で紐を握る形に（名前も） |
| `hands_props/umbrella_shoulder` | 傘を肩にかける | 2026-11-15 | agent | ok |  |
| `hands_props/umbrella_cane` | 閉じた傘に手を置いて立つ | 2026-11-15 | agent | ok |  |
| `hands_props/book_hug` | 本を胸に抱える | 2026-11-15 | agent | ok |  |
| `hands_props/book_offer` | 本を差し出す | 2026-11-15 | agent | ok |  |
| `hands_props/book_under_arm` | 本を脇にかかえる | 2026-11-15 | agent | ok |  |
| `hands_props/pen_notes` | 手帳にペンでメモを取る | 2026-11-15 | agent | ok |  |
| `hands_props/pen_think` | ペンをあごに当てて考える | 2026-11-15 | agent | ok |  |
| `hands_props/sword_rest` | 剣を地面について立つ | 2026-11-15 | agent | ok |  |
| `hands_props/sword_shoulder` | 剣を肩にかつぐ | 2026-11-15 | agent | fixed | 肘の曲がりが範囲の外 → 手を肩から離した |
| `hands_props/sword_ready` | 剣を両手で構える | 2026-11-15 | agent | ok |  |
| `hands_props/bat_shoulder` | バットを肩にかつぐ | 2026-11-15 | agent | fixed | 同上 |
| `hands_props/bat_stance` | バットを構える | 2026-11-15 | agent | ok |  |
| `hands_props/ball_under_arm` | ボールを脇に抱える | 2026-11-15 | agent | ok |  |
| `hands_props/ball_throw_ready` | ボールを投げる構え | 2026-11-15 | agent | fixed | 腕のねじれが範囲の外 → 腕を横に上げ肘を曲げた角度で直接 |
| `hands_props/ball_hold_front` | ボールを両手で前に持つ | 2026-11-15 | agent | ok |  |

## 見て外したもの（作ったが、この体と範囲では自然に描けなかった）

| id | 名前 | 日付 | 見た人 | 理由 |
|---|---|---|---|---|
| `sit_chair/figure_four` | 片足首を膝に乗せて座る | 2026-11-15 | agent | 股関節の外向きのねじれ（範囲 1.0）では、すねを反対の膝の上へ横にできない |
| `sit_floor/w_sit` | 女の子座り | 2026-11-15 | agent | 足の甲を床に付けると足首の範囲の外 |
| `sit_floor/butterfly` | 足の裏を合わせて座る | 2026-11-15 | agent | 足の裏を合わせると足どうしと前腕どうしが重なり、座面が浮いた |
| `sit_floor/side_sit` | 横座り | 2026-11-15 | agent | 脚を横へ流すと座面が床に付かず、重心が支えの外 |
| `sit_floor/side_lean_hand` | 片手をついて横座り | 2026-11-15 | agent | 同上 |
| `sit_floor/legs_out_one_hand` | 片手をついて脚を伸ばす | 2026-11-15 | agent | 手を床につけるには上体を大きく傾ける必要があり、座面が浮いた |
| `kneel_squat/kneel_sit_side` | 膝立ちから腰を落として横を向く | 2026-11-15 | agent | 足首の範囲の外 |
| `everyday/rub_shoulder` | 肩をもむ | 2026-11-15 | agent | 反対の肩へ手を回すと上腕が胸に入るか、肩のねじれが範囲の外 |

横座り・女の子座り・足の裏を合わせた座り・片足首を膝に乗せる座りは、後の群で足首と股関節の扱いを見直してから足す。

## G4 で見て外したもの

| id | 名前 | 日付 | 見た人 | 理由 |
|---|---|---|---|---|
| `lie/side_propped` | 横向きで肘をついて上体を起こす | 2026-11-15 | agent | 肘で上体を起こすと、この体の上腕の長さでは肘が腰より低くなり腰が床から浮く（肘と腰を同時に床に付けるには、下の脚を床へ戻す股関節の内転が範囲〔0.6〕を越える） |
| `relax_bed/side_book` | 横向きに肘をついて本を読む | 2026-11-15 | agent | 同上 |
| `relax_bed/side_talk_propped` | 横向きに肘をついて話す | 2026-11-15 | agent | 同上 |
| `pinup/arms_crossed_lean_forward` | 腕を組んで前かがみに座る | 2026-11-15 | agent | 腕を組んで膝の上へ前かがみになると、男の体で上腕が胸に入った |
| `lie/back_starfish` | 大の字に寝る | 2026-11-15 | agent | 腕と脚を横へ広げると、この体では手足が床に入るか浮くかのどちらかで、胴が床に付かなかった（重心が支えの外） |
| `lie/back_collapsed` | 倒れている | 2026-11-15 | agent | 同上 |
| `lie/front_flat` | うつ伏せに寝る | 2026-11-15 | agent | うつ伏せで頭を横に向けて胴を床に付けると、頭か足先が一番低くなり胴が浮いた（重心が支えの外） |
| `lie/front_arms_pillow` | 腕を枕にうつ伏せで寝る | 2026-11-15 | agent | 同上（前腕の上の頭で胴が浮いた） |
| `lie/front_reach` | うつ伏せで手を前に伸ばす | 2026-11-15 | agent | 同上 |
| `relax_bed/pillow_back` | 枕に頭をのせて仰向けに寝る | 2026-11-15 | agent | 枕の高さに頭を起こすと腰か胸のどちらかが浮いた |
| `relax_bed/front_pillow_hug` | 枕を抱えてうつ伏せに寝る | 2026-11-15 | agent | lie/front_arms_pillow と同じ |
| `relax_bed/back_one_arm_out` | 片腕を投げ出して眠る | 2026-11-15 | agent | lie/back_starfish と同じ |
| `pinup/agura_lean_back` | あぐらで後ろに手をつく | 2026-11-15 | agent | 後ろへ傾けると手が床に届かず、届かせると座面が浮いた |
| `pinup/back_arms_up_knee` | 仰向けで両腕を頭の上に、片膝を立てる | 2026-11-15 | agent | 頭の上の腕が床に入るか肩の範囲の外で、胴が浮いた |
| `pinup/back_head_turned` | 仰向けで顔を横に向けて片腕を上げる | 2026-11-15 | agent | 同上 |
| `pinup/front_arms_folded_chin` | うつ伏せで腕を組んであごをのせる | 2026-11-15 | agent | lie/front_arms_pillow と同じ |
| `pinup/knees_together_hands_between` | 膝をそろえて手を膝の間に | 2026-11-15 | agent | 男の体で手が太ももに、女の体で前腕どうしが重なった |
| `pinup/side_top_leg_forward` | 横向きに肘をついて上の脚を前に出す | 2026-11-15 | agent | lie/side_propped と同じ（確かめは通ったが、絵では腰が浮いて体が斜めに宙に浮いて見えた） |
| `pinup/side_propped_knee_up` | 横向きに肘をつき、上の膝を立てる | 2026-11-15 | agent | 同上 |
| `pinup/side_hand_hip` | 横向きに寝そべって手を腰に | 2026-11-15 | agent | 確かめは通ったが、絵では腰が床から浮いて見えた |

## G4 で G3 の外したものを試し直した結果

- `sit_chair/figure_four`（片足首を膝に乗せて座る）: 戻した（上の椅子に座るの表）。
- `sit_floor/butterfly`（足の裏を合わせて座る）: まだ外したまま。股関節の測り方を直して膝は下ろせたが、足の裏を合わせると足先どうしが重なる（足の形の向きの問題）。
- `sit_floor/w_sit`・`sit_floor/side_sit`・`sit_floor/side_lean_hand`・`sit_floor/legs_out_one_hand`・`kneel_squat/kneel_sit_side`・`everyday/rub_shoulder`: G4 では試していない（外した理由が股関節のねじれではなく、足首の範囲・重心・肩のねじれのため）。

## バッチ C（群 G5、2026-11-16）

- 作り方は G3・G4 と同じ（動作の短い文 → 足と手を置く所・胴の傾き → 関節の角度。どのサイトの絵もデータも使っていない）。道具は scratchpad の poses-g5（`lib_g5.py`・`lib_pair.py`・`poses_s1..s4_*.py`・`poses_t_pairs.py`）。
- 二人のポーズは、一人ずつの確かめに加えて二人の確かめ（SPEC FIGURE-02「二人のポーズ」）を通る。一覧の絵は二人を一緒に回して描く（接地の点は描かない）。
- 持つ物は G5 から描く。G3・G4 の持つポーズも物が描かれるようになった（下の「持つ物を描いて見直したもの」）。

| 分類 | 数 | 計画 §8 |
|---|---|---|
| スポーツ（sports_action） | 38 | 35 |
| アクション（打つ・蹴る・構える・剣・投げる・倒れる）（action） | 30 | 30 |
| ダンス（dance） | 20 | 20 |
| ヒーロー・決めポーズ（heroic） | 20 | 20 |
| 仕事と暮らし（work_life） | 26 | 25 |
| カップル（二人）（couple） | 30 | 30 |
| 二人のポーズ（two_people） | 30 | 30 |
| 計 | 194 | 190 |

## スポーツ（sports_action）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと・気づいたこと |
|---|---|---|---|---|---|
| `sports_action/soccer_kick` | ボールを蹴る（サッカー） | 2026-11-16 | agent | fixed | 軸足を重心の下へ（重心） |
| `sports_action/soccer_trap` | ボールを足で止める | 2026-11-16 | agent | fixed | 上げる脚を高く・軸足を体の下へ（床に触れる・重心） |
| `sports_action/soccer_header` | ヘディングで跳ぶ | 2026-11-16 | agent | ok |  |
| `sports_action/goalkeeper_ready` | ゴールキーパーの構え | 2026-11-16 | agent | ok |  |
| `sports_action/goalkeeper_catch` | ボールを胸で受け止める | 2026-11-16 | agent | ok |  |
| `sports_action/basketball_shoot` | シュートを打つ（バスケットボール） | 2026-11-16 | agent | fixed | つま先立ちの足を後ろへ（重心） |
| `sports_action/basketball_dribble` | ドリブルする | 2026-11-16 | agent | ok |  |
| `sports_action/basketball_pass` | 胸からパスを出す | 2026-11-16 | agent | ok | ボールは持たせていない |
| `sports_action/volleyball_receive` | レシーブする（バレーボール） | 2026-11-16 | agent | ok |  |
| `sports_action/volleyball_spike` | アタックを打つ（宙で） | 2026-11-16 | agent | ok |  |
| `sports_action/volleyball_set` | トスを上げる | 2026-11-16 | agent | ok |  |
| `sports_action/batting_ready` | バットを構える | 2026-11-16 | agent | fixed | 手を前へ（上腕が胴の中）。バットは後ろへ寝る形 |
| `sports_action/batting_contact` | バットを振り出す | 2026-11-16 | agent | fixed | batting_swing（振り抜き）を作り直した: 腕が胴の中・肘の範囲。バットが立つので名前を「バットを振り出す」に |
| `sports_action/pitch_windup` | 足を上げて振りかぶる（投手） | 2026-11-16 | agent | ok |  |
| `sports_action/pitch_release` | ボールを投げ込む（投手） | 2026-11-16 | agent | fixed | 踏み出しを短く（重心） |
| `sports_action/fielder_ready` | 守備の構え（野球） | 2026-11-16 | agent | ok |  |
| `sports_action/tennis_forehand` | フォアハンドで打つ（テニス） | 2026-11-16 | agent | ok |  |
| `sports_action/tennis_serve` | サーブを打つ（テニス） | 2026-11-16 | agent | fixed | 足を平らに（重心） |
| `sports_action/badminton_smash` | スマッシュを打つ（バドミントン） | 2026-11-16 | agent | fixed | 後ろ足を床に（重心） |
| `sports_action/table_tennis_ready` | 卓球の構え | 2026-11-16 | agent | ok |  |
| `sports_action/golf_address` | ゴルフのアドレス | 2026-11-16 | agent | ok |  |
| `sports_action/golf_finish` | ゴルフのフィニッシュ | 2026-11-16 | agent | fixed | 手の位置（肘の範囲・腕のねじれ・上腕が胴の中） |
| `sports_action/arm_stretch` | 腕を胸の前で伸ばすストレッチ | 2026-11-16 | agent | fixed | crouch_start（クラウチングスタート）の代わり: 手が範囲の中で床に届かなかった |
| `sports_action/hurdle` | ハードルを跳び越える | 2026-11-16 | agent | fixed | 後ろ脚の開きを小さく（股関節の範囲・太ももの重なり） |
| `sports_action/long_jump` | 走り幅跳び（宙で） | 2026-11-16 | agent | ok | 同上（宙） |
| `sports_action/shot_put` | 砲丸を構える | 2026-11-16 | agent | fixed | 砲丸の手を首の横へ（肩の範囲・肘の範囲） |
| `sports_action/swim_crawl` | クロールで泳ぐ | 2026-11-16 | agent | fixed | 前の腕を下げる（肩の範囲） |
| `sports_action/speed_skate` | スケートで滑る（片足） | 2026-11-16 | agent | ok |  |
| `sports_action/side_bend` | 体を横に倒すストレッチ | 2026-11-16 | agent | fixed | pushup（腕立て伏せ）の代わり: 脚を範囲の中で床に置けなかった |
| `sports_action/squat_workout` | スクワットをする | 2026-11-16 | agent | fixed | 浅く（足首の範囲） |
| `sports_action/lunge_stretch` | ランジでストレッチ | 2026-11-16 | agent | ok |  |
| `sports_action/jumping_jack` | ジャンピングジャック | 2026-11-16 | agent | ok |  |
| `sports_action/cheer_jump` | チアのジャンプ | 2026-11-16 | agent | ok |  |
| `sports_action/cheer_v` | チアのV字 | 2026-11-16 | agent | ok |  |
| `sports_action/archery_draw` | 弓を引く（弓道・アーチェリー） | 2026-11-16 | agent | fixed | 引く手を肩の前へ（上腕が胴の中・肘の範囲） |
| `sports_action/victory_knee_slide` | ひざで滑ってガッツポーズ | 2026-11-16 | agent | fixed | つま先を立てる（足首の範囲） |
| `sports_action/weight_overhead` | 重りを頭上に差し上げる | 2026-11-16 | agent | ok |  |
| `sports_action/bowling` | ボールを転がす（ボウリング） | 2026-11-16 | agent | fixed | 軸足を体の下へ（重心） |

## アクション（打つ・蹴る・構える・剣・投げる・倒れる）（action）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと・気づいたこと |
|---|---|---|---|---|---|
| `action/fight_stance` | ファイティングポーズ | 2026-11-16 | agent | ok |  |
| `action/jab` | ジャブを打つ | 2026-11-16 | agent | ok |  |
| `action/straight_punch` | ストレートを打つ | 2026-11-16 | agent | ok |  |
| `action/uppercut` | アッパーを突き上げる | 2026-11-16 | agent | ok |  |
| `action/hook` | フックを振る | 2026-11-16 | agent | ok |  |
| `action/cross_block` | 腕を交差させて防ぐ | 2026-11-16 | agent | ok |  |
| `action/high_block` | 腕を上げて頭を守る | 2026-11-16 | agent | ok |  |
| `action/karate_punch` | 正拳突き（前屈立ち） | 2026-11-16 | agent | ok |  |
| `action/palm_strike` | 掌底を突き出す | 2026-11-16 | agent | ok |  |
| `action/front_kick` | 前蹴り | 2026-11-16 | agent | ok |  |
| `action/roundhouse_kick` | 回し蹴り | 2026-11-16 | agent | fixed | 軸足を体の下へ・脚のねじれを小さく（重心・範囲） |
| `action/knee_strike` | 膝蹴り | 2026-11-16 | agent | fixed | 軸足を体の下へ（重心） |
| `action/flying_kick` | 跳び蹴り | 2026-11-16 | agent | ok |  |
| `action/sword_middle` | 剣を中段に構える | 2026-11-16 | agent | ok | 刃が立ち気味（中段より上段寄り） |
| `action/sword_overhead` | 剣を振りかぶる | 2026-11-16 | agent | ok |  |
| `action/sword_cut_down` | 剣を振り下ろす | 2026-11-16 | agent | fixed | 指を下へ向けて刃を前下へ（刃が上を向いていた） |
| `action/sword_thrust` | 剣を立てて踏み込む | 2026-11-16 | agent | fixed | 後ろのかかとを上げる（足首の範囲）。刃が立つので名前を「剣を立てて踏み込む」に |
| `action/sword_side_slash` | 剣を横に払った後 | 2026-11-16 | agent | ok |  |
| `action/iai_draw` | 刀に手をかけて抜く構え | 2026-11-16 | agent | fixed | 柄に掛ける手を前へ（上腕が胴の中） |
| `action/sword_planted` | 剣を地に突き立てて立つ | 2026-11-16 | agent | ok |  |
| `action/throw_overhand` | 大きく振りかぶって投げる | 2026-11-16 | agent | ok |  |
| `action/throw_follow` | 投げ終わり | 2026-11-16 | agent | ok |  |
| `action/throw_underhand` | 下手で放る | 2026-11-16 | agent | ok |  |
| `action/dodge_back` | のけぞってよける | 2026-11-16 | agent | fixed | 反りを小さく・後ろ足を下げる（重心） |
| `action/hit_recoil` | 殴られてよろける | 2026-11-16 | agent | fixed | 後ろ足を平らに（重心） |
| `action/fall_back` | 後ろに倒れかける | 2026-11-16 | agent | ok |  |
| `action/beckon_taunt` | 構えて手招きで挑発する | 2026-11-16 | agent | fixed | fallen_sitting（尻もち）の代わり: 手と腰が床に落ち着かなかった |
| `action/hero_landing` | 片膝をついて着地 | 2026-11-16 | agent | fixed | 拳は床から少し浮かせた（腕が床に届かない）。名前を「片膝をついて着地」に |
| `action/aim_finger_gun` | 指で銃を構える | 2026-11-16 | agent | ok |  |
| `action/crouch_ready` | 低くしゃがんで身構える | 2026-11-16 | agent | fixed | ninja_crouch を作り直した（足首の範囲） |

## ダンス（dance）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと・気づいたこと |
|---|---|---|---|---|---|
| `dance/ballet_first` | バレエの一番ポジション | 2026-11-16 | agent | ok |  |
| `dance/ballet_arabesque` | アラベスク | 2026-11-16 | agent | ok |  |
| `dance/ballet_retire` | つま先立ちで片脚を曲げて回る | 2026-11-16 | agent | ok |  |
| `dance/ballet_plie` | プリエ（膝を開いて曲げる） | 2026-11-16 | agent | fixed | 外への開きを小さく（股関節のねじれ） |
| `dance/jazz_hands` | ジャズハンズ | 2026-11-16 | agent | ok |  |
| `dance/disco_point` | 人差し指を斜め上に突き上げる | 2026-11-16 | agent | ok |  |
| `dance/hiphop_lean` | ヒップホップで腕を組んで反る | 2026-11-16 | agent | ok |  |
| `dance/wave_arms` | 腕でウェーブ | 2026-11-16 | agent | ok |  |
| `dance/robot` | ロボットダンス | 2026-11-16 | agent | fixed | 右腕を体の前でなく横へ（上腕が胴の中） |
| `dance/idol_heart` | アイドルの両手ハート | 2026-11-16 | agent | ok |  |
| `dance/idol_wink_point` | アイドルの決め（指さし） | 2026-11-16 | agent | fixed | 軸足を体の下へ（重心） |
| `dance/flamenco` | フラメンコ | 2026-11-16 | agent | ok |  |
| `dance/hula` | フラを踊る | 2026-11-16 | agent | ok |  |
| `dance/bon_odori` | 盆踊り | 2026-11-16 | agent | ok |  |
| `dance/high_kick_line` | ラインダンスで脚を高く上げる | 2026-11-16 | agent | fixed | 軸足を体の下へ（重心） |
| `dance/spin_arm_up` | 片腕を上げてくるりと回る | 2026-11-16 | agent | fixed | つま先立ちの足を後ろへ（重心） |
| `dance/vogue` | ヴォーグ（手で顔を縁取る） | 2026-11-16 | agent | fixed | 手を遠くへ（肘の範囲） |
| `dance/clap_step` | 手拍子でステップ | 2026-11-16 | agent | fixed | 軸足を体の下へ（重心） |
| `dance/curtsy` | カーテシー（裾をつまんでお辞儀） | 2026-11-16 | agent | ok |  |
| `dance/stage_bow` | 舞台で深くお辞儀する | 2026-11-16 | agent | ok |  |

## ヒーロー・決めポーズ（heroic）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと・気づいたこと |
|---|---|---|---|---|---|
| `heroic/power_stance` | 腰に手を当てて胸を張る | 2026-11-16 | agent | ok |  |
| `heroic/fist_raised` | 拳を天に突き上げる | 2026-11-16 | agent | ok |  |
| `heroic/flying` | 拳を前に突き出して飛ぶ | 2026-11-16 | agent | ok | 同上（宙） |
| `heroic/transform` | 変身ポーズ（腕を斜めに構える） | 2026-11-16 | agent | ok |  |
| `heroic/point_to_sky` | 空を指さして見上げる | 2026-11-16 | agent | ok |  |
| `heroic/point_at_you` | お前だ、と指さす | 2026-11-16 | agent | ok |  |
| `heroic/energy_blast` | 両手のひらから気を放つ | 2026-11-16 | agent | ok |  |
| `heroic/magic_cast` | 片手をかざして魔法を放つ | 2026-11-16 | agent | ok |  |
| `heroic/sword_raised` | 剣を高く掲げる | 2026-11-16 | agent | ok |  |
| `heroic/sword_shoulder_hero` | 剣を下げて堂々と立つ | 2026-11-16 | agent | fixed | 剣が手から下へ下がる形（手のひらが下）。名前を「剣を下げて堂々と立つ」に |
| `heroic/arms_spread_glory` | 両腕を広げて光を浴びる | 2026-11-16 | agent | ok |  |
| `heroic/ready_to_fight` | 拳を握って身構える | 2026-11-16 | agent | ok |  |
| `heroic/back_turned` | 背中を向けて肩越しに振り返る | 2026-11-16 | agent | ok |  |
| `heroic/shield_forearm` | 腕を前に出して受け止める | 2026-11-16 | agent | ok |  |
| `heroic/rising_up` | 片膝から立ち上がる | 2026-11-16 | agent | ok |  |
| `heroic/salute_hero` | 敬礼して立つ | 2026-11-16 | agent | ok |  |
| `heroic/cape_stance` | マントをなびかせて遠くを見る | 2026-11-16 | agent | ok |  |
| `heroic/finger_guns_cool` | 両手の指を銃にしてキメる | 2026-11-16 | agent | ok |  |
| `heroic/kneel_vow` | 片膝をついて誓う | 2026-11-16 | agent | ok |  |
| `heroic/look_up_resolve` | 拳を握って空を見上げる | 2026-11-16 | agent | ok |  |

## 仕事と暮らし（work_life）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと・気づいたこと |
|---|---|---|---|---|---|
| `work_life/typing` | パソコンで文字を打つ | 2026-11-16 | agent | ok |  |
| `work_life/desk_coffee` | 机でコーヒーを飲む | 2026-11-16 | agent | ok |  |
| `work_life/stretch_at_desk` | 椅子で伸びをする | 2026-11-16 | agent | ok |  |
| `work_life/phone_and_notes` | 電話しながらメモを取る | 2026-11-16 | agent | ok |  |
| `work_life/presentation` | 手で示して発表する | 2026-11-16 | agent | ok |  |
| `work_life/business_bow` | 会釈・お辞儀（ビジネス） | 2026-11-16 | agent | ok |  |
| `work_life/card_exchange` | 両手で名刺を差し出す | 2026-11-16 | agent | ok |  |
| `work_life/carry_documents` | 書類を抱えて歩く | 2026-11-16 | agent | ok |  |
| `work_life/check_watch` | 腕時計を見る | 2026-11-16 | agent | ok |  |
| `work_life/cooking_stir` | 鍋をかき混ぜる | 2026-11-16 | agent | ok |  |
| `work_life/cooking_taste` | 小皿で味見する | 2026-11-16 | agent | ok |  |
| `work_life/wipe_table` | テーブルを拭く | 2026-11-16 | agent | ok |  |
| `work_life/hang_laundry` | 洗濯物を干す | 2026-11-16 | agent | fixed | つま先立ちの足を後ろへ（重心） |
| `work_life/sweep_floor` | ほうきで床を掃く | 2026-11-16 | agent | ok |  |
| `work_life/lift_box` | 床から箱を持ち上げる | 2026-11-16 | agent | fixed | 浅く（足首の範囲） |
| `work_life/carry_box` | 箱を胸に抱えて運ぶ | 2026-11-16 | agent | ok |  |
| `work_life/shopping_bags` | 買い物袋を両手に提げる | 2026-11-16 | agent | ok |  |
| `work_life/waiter_tray` | トレーを片手で運ぶ | 2026-11-16 | agent | ok | トレーの形は無い（手のひらを上に） |
| `work_life/garden_dig` | しゃがんで庭の土をいじる | 2026-11-16 | agent | fixed | 肘の向き（腕のねじれ） |
| `work_life/water_plants` | じょうろで水やり | 2026-11-16 | agent | ok |  |
| `work_life/teacher_board` | 黒板に書く | 2026-11-16 | agent | ok |  |
| `work_life/raise_hand_class` | 授業で手を挙げる（座って） | 2026-11-16 | agent | ok |  |
| `work_life/train_strap` | 電車でつり革につかまる | 2026-11-16 | agent | ok |  |
| `work_life/umbrella_walk` | 傘をさして歩く | 2026-11-16 | agent | ok |  |
| `work_life/driving` | 車を運転する | 2026-11-16 | agent | ok |  |
| `work_life/yawn_stretch` | あくびをして伸びる | 2026-11-16 | agent | ok |  |

## カップル（二人）（couple）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと・気づいたこと |
|---|---|---|---|---|---|
| `couple/hold_hands_walk` | 手をつないで歩く | 2026-11-16 | agent | fixed | 二人の間を 3.1 u に、手を間で（上腕・前腕の重なり） |
| `couple/hold_hands_look` | 手をつないで見つめ合う | 2026-11-16 | agent | fixed | 二人の間を 3.1 u に、触れる腕を挙げた |
| `couple/both_hands_face` | 向き合って両手をつなぐ | 2026-11-16 | agent | ok |  |
| `couple/hug` | 抱き合う | 2026-11-16 | agent | fixed | 足を後ろへ・二人の間を 1.35 u に（足の重なり・胴の沈み）、腕どうしの触れを挙げた |
| `couple/back_hug` | 後ろから抱きしめる | 2026-11-16 | agent | fixed | 前の人の手を後ろの人の手の上へ（触れる所が遠かった） |
| `couple/forehead_touch` | 額を寄せ合う | 2026-11-16 | agent | fixed | 足を後ろへ・二人の間を 1.75 u に（足の重なり・頭の沈み） |
| `couple/arm_in_arm` | 腕を組んで歩く | 2026-11-16 | agent | fixed | 二人の間を 2.4 u に、触れる腕を挙げた |
| `couple/shoulder_hold` | 肩を抱き寄せる | 2026-11-16 | agent | fixed | 二人の間を 2.4 u に、手を肩の上へ（手が体の中） |
| `couple/slow_dance` | チークダンス（肩と腰に手を） | 2026-11-16 | agent | fixed | 手を左右に分け腰の外へ（両手が同じ所へ行っていた）・足を後ろへ |
| `couple/waltz` | ワルツの組み方 | 2026-11-16 | agent | fixed | b の上げた手を下げる（肘の範囲）、腕どうしの触れを挙げた |
| `couple/twirl` | 手を取ってくるりと回す | 2026-11-16 | agent | fixed | 二人の間を広げ、b の軸足を体の下へ（重心・腕が体の中） |
| `couple/umbrella_share` | 一つの傘に二人で入る | 2026-11-16 | agent | fixed | 二人の間を 2.7 u に |
| `couple/selfie` | 寄り添って自撮りする | 2026-11-16 | agent | fixed | 肩を抱く腕をやめ、二人の間を 3.1 u に（腕が体の中・肘の範囲） |
| `couple/back_to_back` | 背中合わせで寄りかかる | 2026-11-16 | agent | fixed | heart_together（二人で手のハート）の代わり: 腕が交差して相手の体に入った |
| `couple/proposal` | 片膝をついて手を取る（プロポーズ） | 2026-11-16 | agent | fixed | 二人の間を 3.6 u に（足の重なり） |
| `couple/head_on_shoulder` | 肩に頭をもたせかけて座る | 2026-11-16 | agent | fixed | 傾く向きを直した（体の傾きの符号）・二人の間を 2.4 u に |
| `couple/back_to_back_sit` | 背中合わせで床に座る | 2026-11-16 | agent | ok |  |
| `couple/stand_gaze` | 並んで立ち見つめ合う | 2026-11-16 | agent | fixed | head_pat（頭をなでる）の代わり: 頭が相手の胸に入り手が届かなかった |
| `couple/whisper` | 耳元でささやく | 2026-11-16 | agent | fixed | 手を自分の口元に（腕のねじれ）、二人の間を広げた |
| `couple/cheek_hands` | 両手で頬を包む | 2026-11-16 | agent | fixed | 足を後ろへ（足の重なり） |
| `couple/walk_side_glance` | 並んで歩き見つめ合う | 2026-11-16 | agent | fixed | 二人の間を 3.1 u に |
| `couple/peace_photo` | 並んでピースする | 2026-11-16 | agent | fixed | 二人の間を 3.1 u に |
| `couple/feed_cup` | 飲み物を口元へ差し出す | 2026-11-16 | agent | ok |  |
| `couple/side_hug` | 横から腰を抱く | 2026-11-16 | agent | fixed | 二人の間を 2.8 u に、回す腕は相手の後ろへ |
| `couple/bench_hands` | 並んで座り手を重ねる | 2026-11-16 | agent | fixed | 座った手の高さを直した（膝の上）・二人の間を 3.0 u に |
| `couple/pinky_promise` | 指切りする | 2026-11-16 | agent | ok |  |
| `couple/hands_on_chest` | 胸に手を添えて見上げる | 2026-11-16 | agent | fixed | 手を左右に分けた・腕どうしの触れを挙げた |
| `couple/lean_back` | 後ろの人にもたれかかる | 2026-11-16 | agent | fixed | 手を肩の上へ・二人の間を 1.15 u に（胴の沈み） |
| `couple/piggyback` | おんぶする | 2026-11-16 | agent | fixed | b の手を左右に分けた、脚と腕の触れを挙げた |
| `couple/gaze_walk_lead` | 手を引いて振り返る | 2026-11-16 | agent | ok |  |

## 二人のポーズ（two_people）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと・気づいたこと |
|---|---|---|---|---|---|
| `two_people/handshake` | 握手する | 2026-11-16 | agent | fixed | 手首を握る点から手の長さだけ引いた（手が相手の前腕に重なった） |
| `two_people/high_five` | ハイタッチ | 2026-11-16 | agent | fixed | 手の間を詰めた（触れる所が遠かった） |
| `two_people/fist_bump` | グータッチ | 2026-11-16 | agent | ok |  |
| `two_people/shoulder_buddies` | 肩を組んで笑う | 2026-11-16 | agent | fixed | 二人の間を 2.4 u に、手を肩の後ろへ（手が体の中） |
| `two_people/back_to_back` | 背中合わせに腕を組んで立つ | 2026-11-16 | agent | ok |  |
| `two_people/talking` | 向き合って話す | 2026-11-16 | agent | ok |  |
| `two_people/explain_point` | 指さして教える | 2026-11-16 | agent | fixed | 二人の間を広げた（足・手の重なり） |
| `two_people/sparring` | 向かい合って構える（組手） | 2026-11-16 | agent | ok |  |
| `two_people/sword_clash` | 剣を構えて向き合う | 2026-11-16 | agent | fixed | 剣が交わらず上を向くので名前を「剣を構えて向き合う」に |
| `two_people/bow_greeting` | 向き合ってお辞儀する | 2026-11-16 | agent | ok |  |
| `two_people/hand_over_paper` | 書類を手渡す | 2026-11-16 | agent | fixed | 受け取る手を近づけた（触れる所が遠かった） |
| `two_people/throw_catch` | ボールを投げて受け取る | 2026-11-16 | agent | ok |  |
| `two_people/help_up` | 手を引いて起こす | 2026-11-16 | agent | fixed | 座る人を床に座る形に作り直した（腰と手が床に落ち着かなかった） |
| `two_people/console` | うずくまる友の肩に手を置く | 2026-11-16 | agent | fixed | 手を肩の上へ（手が体の中） |
| `two_people/toast` | カップで乾杯する | 2026-11-16 | agent | ok |  |
| `two_people/secret` | 内緒話をする | 2026-11-16 | agent | fixed | 手を自分の口元に（腕のねじれ）・二人の間を広げた |
| `two_people/push_back` | 背中を押して送り出す | 2026-11-16 | agent | fixed | 手を左右に分けた（腕が交差） |
| `two_people/race` | 並んで走る | 2026-11-16 | agent | fixed | 二人の間を 3.2 u に |
| `two_people/floor_chat` | 床に座っておしゃべり | 2026-11-16 | agent | fixed | 二人の間を 3.6 u に |
| `two_people/teacher_student` | 先生が指し、生徒が手を挙げる | 2026-11-16 | agent | ok |  |
| `two_people/carry_box_together` | 二人で大きな箱を運ぶ | 2026-11-16 | agent | ok |  |
| `two_people/walk_talk` | 並んで歩きながら話す | 2026-11-16 | agent | fixed | pull_by_hand（手を引いて走り出す）の代わり: 後ろへ伸ばす腕が範囲の外 |
| `two_people/photo_friend` | 友だちの写真を撮る | 2026-11-16 | agent | ok |  |
| `two_people/wave_far` | 離れて手を振り合う | 2026-11-16 | agent | ok |  |
| `two_people/cheer_together` | 並んで拳を突き上げる | 2026-11-16 | agent | fixed | 二人の間を 3.1 u に |
| `two_people/punch_dodge` | 拳をかわす | 2026-11-16 | agent | ok |  |
| `two_people/kick_block` | 蹴りを腕で受ける | 2026-11-16 | agent | fixed | 横蹴りを前蹴りに作り直した（片足の重心） |
| `two_people/shoulder_support` | 肩を貸して歩く | 2026-11-16 | agent | fixed | 二人の間を 2.85 u に、回す腕は相手の後ろへ |
| `two_people/thumbs_up_both` | 並んで親指を立てる | 2026-11-16 | agent | fixed | 外側の手で親指を立てる（前腕の重なり） |
| `two_people/look_at_phone` | 並んでスマホをのぞき込む | 2026-11-16 | agent | fixed | 二人の間を 2.9 u に |

## G5 で外したもの

- `action/side_kick`（横蹴り）: 片足で脚を横へ高く上げると重心が軸足から外れ、補正を回すと足が床から離れた。前蹴り・回し蹴りは残した。
- `action/fallen_sitting`（尻もち）・`sports_action/crouch_start`（クラウチングスタート）・`sports_action/pushup`（腕立て伏せ）: 手か腰か足が床に落ち着かなかった（範囲の中で届かない）。代わりに beckon_taunt・arm_stretch・side_bend を入れた。
- 二人: `couple/heart_together`（手を合わせたハート）・`couple/head_pat`（頭をなでる）・`two_people/pull_by_hand`（手を引いて走る）を代わりのポーズに入れ替えた（上の表）。

## 持つ物を描いて見直したもの（G3・G4 のポーズ、2026-11-16、agent）

- 剣・バット・ボール・カップ・スマホ・本・ペン・鞄は、手の中に読める形で出た（hands_props・everyday・sit_chair などの一覧の絵と拡大の絵で見た）。
- `everyday/umbrella`: 手のひらが下を向くポーズで、初めは閉じた傘が手の下へ出た。親指が上を向くときは開くよう規則を直し、開いた傘になった（少し後ろへ傾く）。
- `hands_props/umbrella_cane`（傘を杖のようにつく）: 手のひらが上を向いた手で、傘が床へ向かず横へ出る。ポーズの手の向きの問題として残す（直していない）。
- `hands_props/paper_read` など紙を持つポーズ: 紙が手の横で縦に見える（手のひらの面に沿う）。両手で広げた紙には見えない。残す。
- `hands_props/bat_stance`: バットが後ろへ水平に出る（構えとしては寝すぎ）。残す。

## 体の作り直しに合わせた直し（2026-11-16、agent。SPEC FIGURE-01 の体の作り直し）

体の形を作り直した（胴・肩・股・手足・足・頭。骨格と関節の範囲・確かめの閾値は同じ）。新しい体で全ポーズに確かめをかけ直し、落ちたものと、前の体で床に付いていた所が浮いたものだけを直した。直したのは数だけ（関節の角度、椅子の `seat_h`、二人組の `offset`）で、名前・タグ・手・接地・支え・持つ物は 1 つも変えていない。

- 変わったポーズ: 655 のうち 284（反転は数えない。二人組は 1 項目）。関節の角度 230（そのうち角度だけ 213）、`seat_h` 70（そのうち `seat_h` だけ 53）、二人組 24（そのうち b の置き場所 `offset` 9）。
- なぜ:
  - 座面が上がった（尻が太ももの下へ袋のように垂れていたのをやめ、座った尻の底は太ももの下の面とほぼ同じ高さ: 骨盤から約 0.5 u で、前より約 0.2 u 高い）。椅子のポーズは測り直した `seat_h`（多くは 1.84 → 2.04 u。足を床に置いたまま、椅子が約 0.2 u 高い）。床に座る・寝そべる・もたれるポーズは、足・手・座面が同じ床に付くよう脚と腕の角度を直した（床に座るポーズ 30 すべて、見せポーズ 50、ベッド・ソファ 25 など）。
  - 手足が太くなった（太ももの付け根 0.33 → 0.40 u、膝 0.21 → 0.245、足首 0.12 → 0.142、上腕 0.21 → 0.235、肘 0.15 → 0.165）。確かめのカプセルの半径を新しい太さに測り直したので（閾値は同じ）、組んだ脚・腕組み・手を腿や腰に置くポーズで重なりが限りを超えたものの手足を少し離した。
  - 足の形（つま先が約 0.05 u 長い、かかとが後ろへ出る）: つま先立ち・歩く・蹴るポーズの浮いた足を置き直した。
  - 寝る姿勢: 背中（肩甲骨・尻）と頭の形が変わり、胸郭・頭・かかとが同じ床に付くよう背骨・首・脚を直した。
  - 二人組: 二人の床を合わせ直し（b の `offset` の高さ）、太くなった腕・脚が相手の体へ深く入るものを離した。
- 直し方: 作った時の道具（Python の逆運動学）ではなく、新しい体の網で測りながら関節を少しずつ動かす探索（リポジトリには入れていない: `refit.cpp`・`refit_pair.cpp`。前の体での接地の測りを手本に、付いていた所は付いたまま・浮いていた所は浮いたままにする。動かした量は元のポーズへ戻せるだけ戻す）。1 ポーズで動かした角度の最大は、中央値 0.10 rad、9 割が 0.25 rad 以内、最大 0.70 rad（`pinup/one_knee_chin`: 立てた膝を深く畳んで腰を床へ下ろした）。
- 手で決めたもの（探索が止まったもの）: `pinup/stool_hands_seat`（両腕の x 0.21 → 0.35: 手を座面の前寄りへ、前腕が太ももに入らない）、`two_people/console` の b（肘を 0.15 曲げ、手が太ももに入らない）、`couple/piggyback`（背負われる b の脚を広く〔z 0.55 → 0.9、x 1.35 → 1.2〕・少し高く近く〔offset −1.15, 0.9 → −1.3, 0.86〕、背負う a の腕を外へ〔z 0.41 → 0.6〕）、`sit_floor/meditation`（膝の関節が床から 1.1 u を超えないよう、膝の高さも数えて探し直した）。
- 見たもの: 全分類の一覧の絵を書き直し、床に座る・寝る・見せポーズ・二人組と、大きく動いたポーズ（上の 4 つと `pinup/legs_out_ankles_crossed`・`pinup/knees_up_hair`・`pinup/elbows_recline`・`two_people/floor_chat`・`lie/side_head_on_arm`）を元の角度と並べて見た（同じポーズに見える。腰が床に付き、浮いた足が無い）。全ポーズを 1 つずつ見直してはいない（利用者の目でもまだ見ていない）。

### 2 回目（同じ日。体の硬い線・段を無くした後）

体の 2 回目の直し（腰の横の棚・肩の筋・首の継ぎ目など。SPEC FIGURE-01）で、確かめに落ちた 23 ポーズだけを直した（閾値と関節の範囲は同じ。1 回目と合わせて、元から変わったポーズは 286）。

- なぜ: そけい部と尻・太もものつなぎの幅を広くした（0.16 → 0.3 u）ので、座った底が約 0.04 u ふくらみ、床に座るポーズで足が床から 0.03〜0.06 u 浮いた（限りは 0.03 u）。肩の球を胴から外して形が変わり、相手の肩に置いた手の深さ・届き方が変わった。
- 一人のポーズ 16（関節の角度だけ。最大の変化の中央値 0.2 rad、最大 0.5 rad）: `sit_floor/` hug_knees・face_on_knees・legs_out・legs_apart・knees_up_arms・game・phone・look_back・hug_one_knee・legs_out_crossed_arms・knees_chin、`relax_bed/` wake_stretch・hug_knees_tilt・front_sprawl・sofa_side_lie、`stand/wall_lean_shoulder`（左の上腕が胴へ半径の 0.502 入っていた: 腕の x 0.1885 → 0.2135、肘 1.677 → 1.6895）。
- 二人組 7: `couple/back_to_back_sit`（二人の足）、`two_people/` shoulder_buddies（b の置き場所）・help_up（b の足と高さ）・console（b を 0.18 u 離す）・push_back（0.06 u 近づける）・floor_chat（足）・shoulder_support（置き場所と向き −0.06）。
- 手で決めたもの: `relax_bed/front_sprawl`（うつ伏せで骨盤が床から 0.037 u 浮いた。背骨の x を 0.025 → 0.05 にしてから探索: 腕の x −0.2684 → −0.2184・z ±1.3939 → ±1.4939、脚の x 0.0375 → 0.025）。
- 直し方と道具は 1 回目と同じ（`refit.cpp`・`refit_pair.cpp`、一人は `merge.py`、二人は `merge_pairs.py`）。

### 3 回目（同じ日。顔・首・女の太もも・男の胸の直し）

ポーズのデータは変えていない。655 すべて、そのままで確かめに合格した（閾値・関節の範囲・手足のカプセルの半径も同じ）。

### 4 回目（同じ日。女の内ももの直し）

ポーズのデータは変えていない。655 すべて、そのままで確かめに合格した。
