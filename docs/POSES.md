# ポーズ集の見直しの記録（SPEC FIGURE-02）

人形のポーズ集（`native/src/core/data/figure_poses.json`、genko.poses@1）の全ポーズを、一覧の絵（正面・斜め・横、床の線、接地の点、確かめに落ちたものは赤い枠）で見た記録。絵は `GENKO_POSE_SHEETS=<フォルダ> test_figure_poses` が書く。新しいポーズは、見て ok か直した（fixed）と書いてから入れる。

- 見た人 `agent` はこの版を作ったエージェント（人の目ではない。利用者の目で見直したら、その日付と名前の行を足す）。
- fixed: 絵を見て直したもの（直した点を書く）。ok: そのまま通したもの。
- 自動の確かめ（関節の範囲・接地・重心・体の通り抜け・反転・描ける・小さな絵が同じバイト）は全ポーズが通る（test_figure_poses）。

| 分類 | 数 |
|---|---|
| 立つ（stand） | 46 |
| ファッション（fashion） | 27 |
| 歩く（walk） | 11 |
| 走る（run） | 11 |
| 椅子に座る（sit_chair） | 32 |
| 床に座る（sit_floor） | 30 |
| ひざまずく・しゃがむ（kneel_squat） | 26 |
| 日常の動作（everyday） | 26 |
| 計 | 209 |


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

## 床に座る（sit_floor）

| id | 名前 | 日付 | 見た人 | 結果 | 直したこと |
|---|---|---|---|---|---|
| `sit_floor/seiza` | 正座 | 2026-11-15 | agent | fixed | 足首が床の下へ入っていた → すねを床に沿わせ、足の甲は床のすぐ上（足首が甲より太い形のため 0.13 u ほど浮く） |
| `sit_floor/seiza_bow` | 正座でお辞儀 | 2026-11-15 | agent | fixed | 手が床に届かない → 深く礼をした |
| `sit_floor/seiza_tea` | 正座で湯のみを持つ | 2026-11-15 | agent | ok |  |
| `sit_floor/agura` | あぐら | 2026-11-15 | agent | fixed | 股関節の範囲の中で膝を床まで下ろせない → 膝を少し上げたあぐら（足は反対の膝の下） |
| `sit_floor/agura_chin` | あぐらで頬杖 | 2026-11-15 | agent | fixed | 前へ倒すと座面が浮いた → 背中だけを丸めた |
| `sit_floor/meditation` | 瞑想する | 2026-11-15 | agent | ok |  |
| `sit_floor/agura_book` | あぐらで本を読む | 2026-11-15 | agent | ok |  |
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
