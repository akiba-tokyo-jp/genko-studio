# ポーズ集の見直しの記録（SPEC FIGURE-02）

人形のポーズ集（`native/src/core/data/figure_poses.json`、genko.poses@1）の全ポーズを、一覧の絵（正面・斜め・横、床の線、接地の点、確かめに落ちたものは赤い枠）で見た記録。絵は `GENKO_POSE_SHEETS=<フォルダ> test_figure_poses` が書く（`genko studio figure-poses --sheet DIR` でも書ける）。新しいポーズは、見て ok か直した（fixed）と書いてから入れる。

- 見た人 `agent` はこの版を作ったエージェント（人の目ではない。利用者の目で見直したら、その日付と名前の行を足す）。
- fixed: 絵か確かめを見て直したもの（直した点を書く）。ok: そのまま通したもの。
- 自動の確かめ（関節の範囲・接地・寝た体の面・重心・体の通り抜け・反転・描ける・小さな絵が同じバイト）は全ポーズが通る（test_figure_poses）。
- バッチ A（立つ〜日常の動作）は群 G3、バッチ B（見せポーズ〜物を持つ）と、あぐらの 4 つの作り直しは群 G4。

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
| 計 | 461 | |


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
