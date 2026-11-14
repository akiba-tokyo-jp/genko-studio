#!/usr/bin/env python3
"""変更範囲と工程境界から検証計画を作る。未知の範囲は全体へ拡大する。

フォルダーごとに全体へ送っていた変更（core・storage・service・api・formats・studio・試験の補助とデータ・埋め込みデータ・
名前に系統の無い render）は、ソースの木から機械的に決める（利用者決定 2026-11-14）。試験の「ソースの閉包」（SOURCES から
#include をたどり、ヘッダーが宣言したものを定義する .cpp を足す）に入る変更だけを、その試験に関係するとみなす。子プロセス
として動かす実行物（genko・補助）はその実行物の閉包も足す。説明できない変更は全体へ拡大する。名前の規則（render の系統、
app）は 2026-11-13 までのまま。同じ差分で、前の方針より多くの試験を選ばない。規則と確かめ方は
docs/cpp-migration/VALIDATION.md §2a、Release のビルドとの突き合わせは tools/ci/validation_closure_check.py。
"""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
from typing import Any

PHASES = ('development', 'integration', 'milestone', 'final')
SAFETY = {
    'test_json', 'test_model', 'test_storage', 'test_lock', 'test_journal',
    'test_command_bus', 'test_transaction', 'test_session', 'test_save_e2e',
    'test_migrate_e2e', 'test_gui_save', 'test_app_e2e', 'test_cli_e2e',
    'test_gui_layout', 'test_refusals_e2e',
}


# 写真加工の試験（利用者の決定 2026-11-14:「写真加工のコードを触った工程でだけ ASan にかけるで良いです」）。
# 写真加工のためにある機能の試験で、漫画のページを作るのには使わないもの。差分が PHOTO_PATHS に触れていないとき、final
# 以外の段階の linux-asan の行から外す（Release では毎回流す。規則は make_plan、説明は VALIDATION.md §2b）。
# 迷う試験は入れない: 入れるのは、その試験自身のソースが下の領域だけを確かめているもの。
PHOTO_TESTS = {
    # フィルター（render/fx*: エンジン・種類の表・ギャラリー・変形・レンズと消点・Camera Raw フィルター・描画、窓、コマンドライン）
    'test_fx', 'test_filter_kinds', 'test_fx_gallery', 'test_fx_d2', 'test_fx_lens', 'test_fx_camera_raw', 'test_fx_render',
    'test_contract_filters_pc4', 'test_gui_filters',
    # スマートオブジェクト（中身とリンク・スマートフィルター・スタックモード・上の変形・PSD の置かれた層）
    'test_smart', 'test_smart_decode', 'test_smart_render', 'test_smart_contents', 'test_smart_filters', 'test_psd_smart',
    'test_smart_stack', 'test_smart_warp', 'test_contract_smart', 'test_contract_psd_smart', 'test_contract_smart_links',
    'test_gui_smart', 'test_gui_smart_contents',
    # 色調補正（補正の土台・色の補正・3D LUT・局所の補正・画像の演算）
    'test_adjust', 'test_adjust_color', 'test_lut3d', 'test_tone_ops', 'test_image_calc', 'test_contract_adjust',
    'test_contract_adjust_color', 'test_contract_image_ops', 'test_gui_adjust', 'test_gui_adjust_color', 'test_gui_image_ops',
    # 修復・コンテンツに応じた塗り・マッティング・マジック消しゴム・賢い選択（領域の切り分け）
    'test_inpaint', 'test_retouch', 'test_segment', 'test_matting', 'test_magic_erase', 'test_contract_retouch',
    'test_gui_retouch', 'test_gui_select_mask',
    # カメラ RAW・HDR 統合・パノラマ・自動整列・スキャンの切り抜き
    'test_raw', 'test_contract_raw', 'test_gui_camera_raw', 'test_hdr_merge', 'test_scancrop', 'test_contract_hdr',
    'test_features', 'test_panorama', 'test_contract_auto_align',
    # ビデオの素材とアニメーション
    'test_footage', 'test_contract_video', 'test_contract_anim', 'test_gui_anim',
    # 制作の補助（レイヤーカンプ・スライス・変数とデータセット・自動処理・計測・アートボード）
    'test_layer_comps', 'test_contract_layer_comps', 'test_gui_layer_comps', 'test_slices', 'test_contract_slices',
    'test_gui_slices', 'test_variables', 'test_contract_variables', 'test_gui_variables', 'test_automate',
    'test_contract_automate', 'test_gui_automate', 'test_measure', 'test_measure_features', 'test_contract_measure',
    'test_gui_measure', 'test_contract_artboards', 'test_gui_artboards',
    # プラグイン（Python・native・WebAssembly、プラグインのホスト）
    'test_contract_plugins', 'test_contract_wasm_plugins', 'test_native_plugins', 'test_wasm_plugins',
    # .pcraft の形式
    'test_pcraft', 'test_contract_pcraft',
}
# 写真加工のコード（パスの先頭の一致。「.」で終わる項は、その名前のファイルだけ）。上の領域のためにある製品のソースと、
# PHOTO_TESTS の試験自身のソース・補助・データ。ここに触れた差分では、写真加工の試験を ASan から外さない。
PHOTO_PATHS = (
    # core
    'native/src/core/adjust', 'native/src/core/color_adjust.', 'native/src/core/filter_kinds.', 'native/src/core/filter_upstream.',
    'native/src/core/filters.', 'native/src/core/gallery_kinds.', 'native/src/core/lens.', 'native/src/core/vanishing.',
    'native/src/core/exif.', 'native/src/core/lut3d.', 'native/src/core/exposure.', 'native/src/core/raw_develop.',
    'native/src/core/smart.', 'native/src/core/anim.', 'native/src/core/ops_anim.', 'native/src/core/video.',
    'native/src/core/layer_comps.', 'native/src/core/slices.', 'native/src/core/slice_layout.', 'native/src/core/variables.',
    'native/src/core/measure.', 'native/src/core/artboards.', 'native/src/core/image_assets.', 'native/src/core/production_json.',
    # render: フィルター
    'native/src/render/fx', 'native/src/render/filters.', 'native/src/render/render_picture.', 'native/src/render/vanishing.',
    'native/src/render/ops_filters.', 'native/src/render/psd_filters.',
    # render: スマートオブジェクト
    'native/src/render/smart', 'native/src/render/ops_smart', 'native/src/render/psd_smart.', 'native/src/render/stack.',
    # render: 色調補正
    'native/src/render/adjust', 'native/src/render/ops_adjust.', 'native/src/render/ops_calc.', 'native/src/render/ops_fade.',
    'native/src/render/tone_ops.', 'native/src/render/trap.', 'native/src/render/psd_adjust',
    # render: 修復・マッティング・賢い選択
    'native/src/render/inpaint.', 'native/src/render/poisson.', 'native/src/render/content_aware.', 'native/src/render/matting.',
    'native/src/render/ops_matting.', 'native/src/render/ops_retouch.', 'native/src/render/retouch_dabs.',
    'native/src/render/erase_magic.', 'native/src/render/segment/', 'native/src/render/select_smart.',
    # render: RAW・HDR・パノラマ・自動整列・スキャン
    'native/src/render/raw.', 'native/src/render/ops_camera_raw.', 'native/src/render/hdr_merge.', 'native/src/render/hdr_preview.',
    'native/src/render/panorama.', 'native/src/render/pyramid.', 'native/src/render/features.', 'native/src/render/scancrop.',
    'native/src/render/ops_align.',
    # render: ビデオ・アニメーション
    'native/src/render/footage.', 'native/src/render/ops_video.', 'native/src/render/anim.', 'native/src/render/movie.',
    'native/src/render/timelapse.',
    # render: 制作の補助
    'native/src/render/ops_layer_comps.', 'native/src/render/ops_slices.', 'native/src/render/ops_variables.',
    'native/src/render/ops_measure.', 'native/src/render/measure_features.', 'native/src/render/ops_artboards.',
    'native/src/render/ops_production.', 'native/src/render/psd_artboards.', 'native/src/render/psd_comps.',
    'native/src/render/psd_slices.', 'native/src/render/psd_production.',
    # render: プラグイン・.pcraft
    'native/src/render/plugins.', 'native/src/render/plugin_bands.', 'native/src/render/pcraft', 'native/src/render/blake3.',
    'native/src/render/zstd.',
    # formats・service
    'native/src/formats/artboards.', 'native/src/formats/datasets.', 'native/src/formats/psd_adjust.', 'native/src/formats/pcraft_write.',
    'native/src/formats/image_assets.', 'native/src/formats/smart_files.',
    'native/src/service/actions.', 'native/src/service/automate', 'native/src/service/script_events.',
    # app: 上の領域の窓・パネル・メニュー
    'native/src/app/adjust_forms', 'native/src/app/adjust_panels.', 'native/src/app/image_dialogs.', 'native/src/app/main_window_adjust.',
    'native/src/app/filter_forms', 'native/src/app/filter_gallery_window.', 'native/src/app/filter_menu.', 'native/src/app/blur_gallery_tool.',
    'native/src/app/lens_window.', 'native/src/app/lighting_window.', 'native/src/app/vanishing_window.',
    'native/src/app/smart_panel.', 'native/src/app/layer_panel_smart_filters.', 'native/src/app/main_window_smart.',
    'native/src/app/main_window_smart_select.', 'native/src/app/select_mask_window.', 'native/src/app/retouch_tools.',
    'native/src/app/clone_source_panel.', 'native/src/app/content_aware_window.',
    'native/src/app/camera_raw', 'native/src/app/main_window_raw.', 'native/src/app/hdr_dialog.', 'native/src/app/photomerge_dialog.',
    'native/src/app/main_window_photomerge.', 'native/src/app/auto_align_dialog.',
    'native/src/app/timeline.', 'native/src/app/main_window_anim.', 'native/src/app/main_window_video.',
    'native/src/app/layer_comps_panel.', 'native/src/app/main_window_layer_comps.', 'native/src/app/canvas_slices.',
    'native/src/app/main_window_slices.', 'native/src/app/variables_dialog.', 'native/src/app/main_window_variables.',
    'native/src/app/automate_dialogs.', 'native/src/app/main_window_automate.', 'native/src/app/canvas_measure.',
    'native/src/app/main_window_measure.', 'native/src/app/measurement_log', 'native/src/app/artboard_dialog.',
    'native/src/app/main_window_artboards.', 'native/src/app/main_window_production.', 'native/src/app/plugins_dialog.',
    # プラグインのホスト
    'native/wasm-host/',
    # 写真加工の試験自身のソース・補助・データ
    'native/tests/unit/test_fx', 'native/tests/unit/test_filter_kinds.', 'native/tests/contract/test_contract_filters_pc4.',
    'native/tests/gui/test_gui_filters.',
    'native/tests/unit/test_smart', 'native/tests/unit/test_psd_smart.', 'native/tests/contract/test_contract_smart',
    'native/tests/contract/test_contract_psd_smart.', 'native/tests/gui/test_gui_smart',
    'native/tests/unit/test_adjust', 'native/tests/unit/test_lut3d.', 'native/tests/unit/test_tone_ops.', 'native/tests/unit/test_image_calc.',
    'native/tests/contract/test_contract_adjust', 'native/tests/contract/test_contract_image_ops.', 'native/tests/gui/test_gui_adjust',
    'native/tests/gui/test_gui_image_ops.',
    'native/tests/unit/test_inpaint.', 'native/tests/unit/test_retouch.', 'native/tests/unit/test_segment.', 'native/tests/unit/test_matting.',
    'native/tests/unit/test_magic_erase.', 'native/tests/contract/test_contract_retouch.', 'native/tests/gui/test_gui_retouch.',
    'native/tests/gui/test_gui_select_mask.',
    'native/tests/unit/test_raw.', 'native/tests/contract/test_contract_raw.', 'native/tests/gui/test_gui_camera_raw.',
    'native/tests/unit/test_hdr_merge.', 'native/tests/unit/test_scancrop.', 'native/tests/contract/test_contract_hdr.',
    'native/tests/unit/test_features.', 'native/tests/unit/test_panorama.', 'native/tests/contract/test_contract_auto_align.',
    'native/tests/unit/test_footage.', 'native/tests/contract/test_contract_video.', 'native/tests/contract/test_contract_anim.',
    'native/tests/contract/anim_cases.json', 'native/tests/gui/test_gui_anim.',
    'native/tests/unit/test_layer_comps.', 'native/tests/contract/test_contract_layer_comps.', 'native/tests/gui/test_gui_layer_comps.',
    'native/tests/unit/test_slices.', 'native/tests/contract/test_contract_slices.', 'native/tests/gui/test_gui_slices.',
    'native/tests/unit/test_variables.', 'native/tests/contract/test_contract_variables.', 'native/tests/gui/test_gui_variables.',
    'native/tests/unit/test_automate.', 'native/tests/contract/test_contract_automate.', 'native/tests/gui/test_gui_automate.',
    'native/tests/unit/test_measure', 'native/tests/contract/test_contract_measure.', 'native/tests/gui/test_gui_measure.',
    'native/tests/contract/test_contract_artboards.', 'native/tests/gui/test_gui_artboards.',
    'native/tests/contract/test_contract_plugins.', 'native/tests/unit/test_native_plugins.', 'native/tests/unit/test_wasm_plugins.',
    'native/tests/support/native_test_plugin.', 'native/tests/data/plugins/',
    'native/tests/unit/test_pcraft.', 'native/tests/contract/test_contract_pcraft.', 'native/tests/data/pcraft/',
    'native/tests/support/raw_testgen.', 'native/tests/support/psd_build.', 'native/tests/support/comma_locale.',
)


def inventory(root):
    text = (Path(root) / 'native/tests/CMakeLists.txt').read_text(encoding='utf-8')
    result = {'normal': set(), 'contract': set(), 'perf': set(), 'gpu': set()}
    for name, body in re.findall(r'genko_test\((test_\w+)\s+(.*?)\)', text, re.S):
        kind = 'perf' if re.search(r'LABELS\s+perf\b', body) else 'contract' if re.search(r'LABELS\s+contract\b', body) else 'normal'
        result[kind].add(name)
    # (PC5 H-a: a GUI test run again on Xvfb with Mesa's OpenGL — the graphics card's way —, Linux only; in a plan only when
    # asked for: make_plan(gpu=True), --gpu)
    result['gpu'] |= set(re.findall(r'genko_xvfb_test\((test_\w+)\s', text))
    if not result['normal'] or not result['contract']:
        raise ValueError('CTest台帳を抽出できません')
    return result


# ---- ソースの木の読み取り（閉包・子プロセス・データ） -------------------------------------------------------------
# どの規則も、木に置かれた文字だけで決まる（ビルドの前に計画を作る）。パスは / 区切り・大文字小文字そのまま、改行は LF に
# そろえるので、Linux と Windows の checkout で同じ答えになる。

IMPLEMENTATION_SUFFIXES = ('.cpp', '.c', '.cc', '.cxx')
CODE_SUFFIXES = IMPLEMENTATION_SUFFIXES + ('.hpp', '.h', '.hh', '.hxx', '.inc', '.ipp', '.tpp')
CODE_ROOTS = ('native/src', 'native/tests')
# (#include "…" is looked for beside the including file, then here: genko_library's include root and the test support's)
INCLUDE_ROOTS = ('native/src', 'native/tests/support')

_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"\n]+)"', re.M)
_IDENT = re.compile(r'[A-Za-z_]\w*')
_NOT_A_DEFINITION = r'(?!(?:static|namespace|using|typedef|return|else|if|for|while|switch|case|do)\b)'
# (a definition at the start of a line: `Type ns::Class::name(`; group 1 its qualifiers, group 2 its name)
_DEFINED_FUNCTION = re.compile(r'^(?=[A-Za-z_])' + _NOT_A_DEFINITION + r'[^\n;(){}=]*?(?<![A-Za-z0-9_])'
                               r'((?:[A-Za-z_]\w*[ \t]*(?:<[^\n;(){}]*>)?[ \t]*::[ \t]*)*)(~?[A-Za-z_]\w*)[ \t]*\(', re.M)
_DEFINED_OPERATOR = re.compile(r'^(?=[A-Za-z_])' + _NOT_A_DEFINITION + r'[^\n;{}]*?(?<![A-Za-z0-9_])'
                               r'((?:[A-Za-z_]\w*[ \t]*(?:<[^\n;(){}]*>)?[ \t]*::[ \t]*)*)operator\b', re.M)
_DEFINED_VALUE = re.compile(r'^(?=[A-Za-z_])' + _NOT_A_DEFINITION + r'[^\n;(){}=]*?(?<![A-Za-z0-9_])'
                            r'((?:[A-Za-z_]\w*[ \t]*::[ \t]*)*)([A-Za-z_]\w*)[ \t]*(?:\[[^\]\n]*\][ \t]*)?(?:=|\{|;)', re.M)
_CALLED = re.compile(r'([A-Za-z_]\w*)[ \t]*\(')
_SCOPE = re.compile(r'\b(?:class|struct|union|namespace)\b([^;{}()]*)\{')
_MOC = re.compile(r'\bQ_(?:OBJECT|GADGET|NAMESPACE)\b')
_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
_STRIPPED = re.compile(r'R"([^()\s"]*)\(.*?\)\1"|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
_PROGRAM_MACRO = re.compile(r'\b([A-Za-z_]\w*)=\\?"\$<TARGET_FILE(_DIR)?:(\w+)>')


def normal_path(path):
    """/ 区切りで . と .. を畳んだパス。木の外へ出るなら None。"""
    out = []
    for part in str(path).replace('\\', '/').split('/'):
        if part in ('', '.'):
            continue
        if part == '..':
            if not out:
                return None
            out.pop()
        else:
            out.append(part)
    return '/'.join(out)


def cmake_code(text):
    """コメントを外したCMakeの文字（引用符の中の # は残す）。"""
    lines = []
    for line in text.split('\n'):
        quoted = False
        for i, c in enumerate(line):
            if c == '"' and (i == 0 or line[i - 1] != '\\'):
                quoted = not quoted
            elif c == '#' and not quoted:
                line = line[:i]
                break
        lines.append(line)
    return '\n'.join(lines)


def cmake_commands(code):
    """コメントを外したCMakeの文字から (命令, 引数の文字, 始まり, 終わり) を順に返す。"""
    return [(m.group(1), m.group(2), m.start(), m.end()) for m in re.finditer(r'(?<![\w.$])([A-Za-z_]\w*)[ \t]*\(((?:[^()]|\([^()]*\))*)\)', code)]


def library_of(path):
    """ライブラリの単位: native/src/<名前>、試験の木は native/tests。"""
    parts = path.split('/')
    return '/'.join(parts[:3]) if parts[:2] == ['native', 'src'] else '/'.join(parts[:2])


def enclosing_names(text, tokens, seeds, classes):
    """text の中で tokens のどれかを使っている関数・クラス・変数の名前。返すのは (名前の集合, 持ち主の分からない使用があるか)。

    宣言だけ（本体も = も無い文）は使用に数えない。ただし seeds（実行物を指すマクロ）は、どこに出ても使用。
    """
    names = set()
    unknown = False
    lines = []
    for line in text.split('\n'):
        define = re.match(r'[ \t]*#[ \t]*define[ \t]+(\w+)(.*)', line)
        if define:
            if set(_IDENT.findall(define.group(2))) & tokens:
                names.add(define.group(1))
            line = ''
        elif re.match(r'[ \t]*#', line):
            line = ''
        lines.append(line)
    code = _STRIPPED.sub(' ', '\n'.join(lines))
    wanted = re.compile(r'[{};]|(?<![A-Za-z0-9_])(?:' + '|'.join(re.escape(t) for t in sorted(tokens)) + r')(?![A-Za-z0-9_])')
    stack = []  # (the text before the block's brace, back to the statement's start)
    boundary = 0

    def owner_of(header):
        """(名前 または None, 素通しの囲みか)"""
        if re.search(r'\bnamespace\b', header) or re.fullmatch(r'\s*extern\s*', header):
            return None, True
        if '(' in header:
            declarator = header[:header.index('(')]
            if '=' in declarator:  # (a variable set from a lambda or a call)
                found = _IDENT.findall(declarator[:declarator.index('=')])
                return (found[-1] if found else None), False
            qualified = re.search(r'([A-Za-z_]\w*)\s*(?:<[^<>]*>)?\s*::\s*~?[A-Za-z_]\w*\s*$', declarator)
            if qualified and qualified.group(1) in classes:
                return qualified.group(1), False
            found = _IDENT.findall(declarator)
            return (found[-1] if found else None), False
        kind = re.search(r'\b(?:class|struct|union)\b([^:]*)', header)
        if kind:
            found = [n for n in _IDENT.findall(kind.group(1)) if n != 'final']
            return (found[-1] if found else None), False
        if '=' in header:
            found = _IDENT.findall(header[:header.index('=')])
            return (found[-1] if found else None), False
        return None, False

    for match in wanted.finditer(code):
        token = match.group(0)
        if token == '{':
            stack.append(code[boundary:match.start()])
            boundary = match.end()
        elif token == '}':
            if stack:
                stack.pop()
            boundary = match.end()
        elif token == ';':
            boundary = match.end()
        else:
            owner = None
            inside = False
            for header in stack:
                name, transparent = owner_of(header)
                if not transparent:
                    owner, inside = name, True
                    break
            if inside:
                if owner is None:
                    unknown = True
                else:
                    names.add(owner)
                continue
            # 囲みの外の文: 定義（= がある・本体が続く）なら、その名前が使っている。
            end = re.compile(r'[;{]').search(code, match.end())
            before = code[boundary:match.start()]
            if '=' in before:
                head = before[:before.index('=')]
                if '(' in head:  # (a default argument of a function declared here)
                    found = _IDENT.findall(head[:head.index('(')])
                else:
                    found = _IDENT.findall(head)
                if found:
                    names.add(found[-1])
                else:
                    unknown = True
            elif end and end.group(0) == '{' and '(' in before:
                # (an argument's default or an initialiser of the function defined here)
                found = _IDENT.findall(before[:before.index('(')])
                if found:
                    names.add(found[-1])
                else:
                    unknown = True
            elif token in seeds:
                unknown = True
    return names, unknown


class SourceTree:
    """native/src と native/tests の今の木: インクルード、実装、試験と実行物の登録、データの埋め込み。"""

    def __init__(self, root):
        self.root = Path(root)
        self.text = {}
        self.cmake = {}
        self.present = set()
        for top in CODE_ROOTS:
            base = self.root / top
            if not base.is_dir():
                continue
            for folder, folders, names in os.walk(base):
                folders.sort()
                for name in sorted(names):
                    path = normal_path(Path(folder, name).relative_to(self.root).as_posix())
                    self.present.add(path)
                    if name.endswith(CODE_SUFFIXES):
                        self.text[path] = self.read(path)
                    elif name == 'CMakeLists.txt':
                        self.cmake[path] = cmake_code(self.read(path))
        self._register()
        self._graph()
        self._reach = {}
        self._literals = None
        self._programs_of = None

    def read(self, path):
        # (bytes, then LF: a checkout with CRLF reads the same)
        return (self.root / path).read_bytes().decode('utf-8', 'replace').replace('\r\n', '\n').replace('\r', '\n')

    # -- 登録（CMake） --

    def _register(self):
        self.tests = {}        # 試験 → SOURCES
        self.programs = {}     # 実行物（試験でない add_executable） → ソース
        self.macros = {}       # コンパイル定義の名前 → それが指す実行物（ソースの分からないものは None）
        self.depends = {}      # 試験 → add_dependencies で求めた実行物
        suite = self.cmake.get('native/tests/CMakeLists.txt', '')
        more = {}
        for path, code in sorted(self.cmake.items()):
            folder = path.rsplit('/', 1)[0]
            for command, body, _, _ in cmake_commands(code):
                words = body.split()
                if command == 'genko_test' and words and path == 'native/tests/CMakeLists.txt':
                    sources = re.search(r'\bSOURCES\s+(.*?)(?:\s(?:DEPS|LABELS|TIMEOUT|GUI)\b|$)', body, re.S)
                    self.tests[words[0]] = sorted(filter(None, (normal_path(folder + '/' + s) for s in (sources.group(1).split() if sources else []))))
                elif command == 'add_executable' and words:
                    self.programs[words[0]] = sorted(filter(None, (normal_path(folder + '/' + s) for s in words[1:] if s.endswith(CODE_SUFFIXES))))
                elif command == 'add_dependencies' and words:
                    self.depends.setdefault(words[0], set()).update(words[1:])
                elif command == 'target_sources' and words:
                    more.setdefault(words[0], set()).update(filter(None, (normal_path(folder + '/' + s) for s in words[1:] if s.endswith(CODE_SUFFIXES))))
        for target, sources in more.items():   # (target_sources(test …): more sources of a test or a program)
            for registry in (self.tests, self.programs):
                if target in registry:
                    registry[target] = sorted(set(registry[target]) | sources)
        for name, directory, target in _PROGRAM_MACRO.findall(suite):
            # ($<TARGET_FILE_DIR:genko>/…: a program beside genko whose sources this tree does not hold — the plugin host)
            self.macros[name] = None if directory else target
        self.executable_sources = {s for sources in list(self.tests.values()) + list(self.programs.values()) for s in sources}

    # -- 閉包のグラフ --

    def _graph(self):
        self.includes = {}
        self.unresolved = {}   # (quoted includes found in no root: Qt's .moc files, the vendored libraries' headers)
        for path, text in self.text.items():
            found = set()
            for name in _INCLUDE.findall(text):
                for base in (path.rsplit('/', 1)[0],) + INCLUDE_ROOTS:
                    target = normal_path(base + '/' + name)
                    if target in self.text:
                        found.add(target)
                        break
                else:
                    self.unresolved.setdefault(path, set()).add(name)
            self.includes[path] = found
        # ヘッダーが宣言した名前を、同じライブラリのどの実装ファイルが行頭で定義しているか。
        defined = {}
        for path, text in self.text.items():
            if not path.endswith(IMPLEMENTATION_SUFFIXES) or path in self.executable_sources:
                continue
            library = library_of(path)
            for match in _DEFINED_FUNCTION.finditer(text):
                qualifiers = _IDENT.findall(match.group(1))
                defined.setdefault((library, match.group(2).lstrip('~')), set()).add((path, qualifiers[-1] if qualifiers else None))
            for match in _DEFINED_OPERATOR.finditer(text):
                qualifiers = _IDENT.findall(match.group(1))
                defined.setdefault((library, 'operator'), set()).add((path, qualifiers[-1] if qualifiers else None))
            for match in _DEFINED_VALUE.finditer(text):
                qualifiers = _IDENT.findall(match.group(1))
                defined.setdefault((library, '=' + match.group(2)), set()).add((path, qualifiers[-1] if qualifiers else None))
        self.implements = {}
        moc = {}
        for path, text in self.text.items():
            if path.endswith(IMPLEMENTATION_SUFFIXES):
                continue
            library = library_of(path)
            names = set(_IDENT.findall(text))
            declared = set(_CALLED.findall(text)) | {'=' + n for n in names} | ({'operator'} if 'operator' in names else set())
            scopes = set()
            for match in _SCOPE.finditer(text):
                scopes.update(_IDENT.findall(match.group(1)))
            found = set()
            for name in declared:
                for implementation, qualifier in defined.get((library, name), ()):
                    if qualifier is None or qualifier in scopes:
                        found.add(implementation)
            self.implements[path] = found
            # AUTOMOC: 一つのライブラリの Q_OBJECT のヘッダーは、一つの生成物（mocs_compilation）にまとまって結ばれる。
            if path.startswith('native/src/') and _MOC.search(text):
                moc.setdefault(library, set()).add(path)
        self.moc = moc
        self.forward = {}
        for path in self.text:
            nodes = set(self.includes[path]) | self.implements.get(path, set())
            self.forward[path] = nodes
        for library, headers in moc.items():
            node = 'moc:' + library
            self.forward[node] = set(headers)
            for header in headers:
                self.forward[header].add(node)
        self.backward = {}
        for path, nodes in self.forward.items():
            for node in nodes:
                self.backward.setdefault(node, set()).add(path)
        self.implemented = {i for found in self.implements.values() for i in found}

    def closure(self, seeds):
        """seeds から届くファイル（インクルード・実装・moc のまとまり）。"""
        seen = set(s for s in seeds if s in self.forward)
        stack = list(seen)
        while stack:
            for node in self.forward.get(stack.pop(), ()):
                if node not in seen:
                    seen.add(node)
                    stack.append(node)
        return {p for p in seen if not p.startswith('moc:')}

    def reaching(self, path):
        """path に届くファイル（path 自身を含む）。"""
        if path not in self._reach:
            seen = {path}
            stack = [path]
            while stack:
                for node in self.backward.get(stack.pop(), ()):
                    if node not in seen:
                        seen.add(node)
                        stack.append(node)
            self._reach[path] = seen
        return self._reach[path]

    def unexplained(self, path):
        """どのヘッダーからも連れて来られない実装ファイル（実行物のソースでもない）: 届き方を説明できない。"""
        return path.endswith(IMPLEMENTATION_SUFFIXES) and path not in self.executable_sources and path not in self.implemented

    # -- 子プロセス --

    def programs_of(self):
        """試験 → 子プロセスとして動かす実行物。add_dependencies と、実行物を指すマクロの使用（補助関数ごしを含む）から。"""
        if self._programs_of is not None:
            return self._programs_of
        result = {name: {d for d in self.depends.get(name, ()) if d in self.programs} for name in self.tests}
        # (the helpers the tests share: not a test's or a program's own source)
        shared = sorted(p for p in self.text if p.startswith('native/tests/') and p not in self.executable_sources)
        started_by = {}   # 実行物 → それを動かす実行物（補助が genko を起こす、など）
        classes = set()
        for path in shared:
            for match in re.finditer(r'\b(?:class|struct|union)\s+([A-Za-z_]\w*)[^;{()]*\{', self.text[path]):
                classes.add(match.group(1))
        self.carriers = {}
        for program in sorted(set(self.macros.values()) - {None}):
            seeds = {m for m, target in self.macros.items() if target == program}
            carriers = set(seeds)
            carrier_files = set()
            while True:
                used = re.compile(r'(?<![A-Za-z0-9_])(?:' + '|'.join(sorted(carriers)) + r')(?![A-Za-z0-9_])')
                grown = set(carriers)
                for path in shared:
                    if used.search(self.text[path]):
                        names, unknown = enclosing_names(self.text[path], carriers, seeds, classes)
                        grown |= names
                        if unknown:
                            carrier_files.add(path)
                if grown == carriers:
                    break
                carriers = grown
            self.carriers[program] = (sorted(carriers), sorted(carrier_files))
            reached = set()
            for path in carrier_files:
                reached |= self.reaching(path)
            def starts(sources):
                return bool(reached & set(sources)) or any(used.search(self.text[s]) for s in sources if s in self.text)
            for name, sources in self.tests.items():
                if starts(sources):
                    result[name].add(program)
            for other, sources in self.programs.items():
                if other != program and starts(sources):
                    started_by.setdefault(other, set()).add(program)
        for found in result.values():   # (a helper that starts another program: the test runs both)
            while True:
                more = {p for helper in found for p in started_by.get(helper, ())} - found
                if not more:
                    break
                found |= more
        self._programs_of = result
        return result

    def tests_reaching(self, path):
        """path が、自分の閉包か、子プロセスとして動かす実行物の閉包に入る試験。"""
        reach = self.reaching(path)
        programs = {name for name, sources in self.programs.items() if reach & set(sources)}
        spawned = self.programs_of()
        return {name for name, sources in self.tests.items() if reach & set(sources) or spawned[name] & programs}

    # -- データ --

    def consumers_of_template(self, template):
        """CMake が埋める雛形（*.cpp.in）の定義名を使うソース。名前を読み取れないなら None。"""
        text = self.read(template)
        block = r'\bextern\s+const\b[^;=]*?\b([A-Za-z_]\w*)\s*(?:\[\s*\])?\s*=\s*(?:\{\s*@\w+@\s*\}|[^;@]*);'
        names = set(re.findall(block, text))
        if not names or re.search(r'@\w+@', re.sub(block, '', text)):
            return None
        wanted = re.compile(r'(?<![A-Za-z0-9_])(?:' + '|'.join(sorted(names)) + r')(?![A-Za-z0-9_])')
        return {path for path, code in self.text.items() if path.startswith('native/src/') and wanted.search(code)}

    def embedded(self, path):
        """path を埋め込む雛形。返すのは (雛形の集合, CMake に書かれているのに読み取れなかったか)。"""
        templates = set()
        unreadable = False
        for cmake, code in sorted(self.cmake.items()):
            if not cmake.startswith('native/src/'):
                continue
            folder = cmake.rsplit('/', 1)[0]
            spelled = set()
            if path.startswith(folder + '/'):
                spelled.add(path[len(folder) + 1:])
            if path.startswith('native/'):
                spelled.add('${GENKO_SOURCE_ROOT}/' + path[len('native/'):])
            commands = cmake_commands(code)
            for spelling in sorted(spelled):
                for match in re.finditer(r'(?<![\w.\-])' + re.escape(spelling) + r'(?![\w.\-/])', code):
                    inside = [c for c, _, start, end in commands if start <= match.start() < end]
                    if inside and inside[0] in ('qt_add_resources', 'qt_add_big_resources'):
                        continue  # (a .qrc named to Qt's resource compiler: the qrc rule)
                    following = [body for c, body, start, _ in commands if start > match.start() and c == 'configure_file']
                    words = following[0].split() if following else []
                    template = normal_path(folder + '/' + words[0].strip('"')) if words else None
                    if template and template.endswith('.in') and template in self.present:
                        templates.add(template)
                    else:
                        unreadable = True
        return templates, unreadable

    def resources(self):
        """qrc の名前 → (qrc のパス, そこに並ぶファイル)。"""
        result = {}
        base = self.root / 'native/resources'
        if base.is_dir():
            for file in sorted(base.glob('*.qrc')):
                path = 'native/resources/' + file.name
                listed = {normal_path('native/resources/' + name.strip()) for name in re.findall(r'<file\b[^>]*>([^<]+)</file>', self.read(path))}
                result[file.name[:-len('.qrc')]] = (path, listed - {None})
        return result

    def resource_users(self, name):
        """Q_INIT_RESOURCE(name) を書いているソース。"""
        wanted = re.compile(r'\bQ_INIT_RESOURCE\s*\(\s*' + re.escape(name) + r'\s*\)')
        return {path for path, code in self.text.items() if 'Q_INIT_RESOURCE' in code and wanted.search(code)}

    def literal_index(self):
        """試験の木のソースに書かれた文字列の索引。

        names: / の無い文字列 → ファイル。singles: 部品が一つだけの文字列（/ の有無によらず） → ファイル。
        paths: 先頭の部品 → [(二つ以上の部品, / で終わるか, ファイル)]。ending: 二つ以上の部品の文字列の最後の部品 → ファイル。
        """
        if self._literals is None:
            names, singles, paths, ending = {}, {}, {}, {}
            for file in sorted(p for p in self.text if p.startswith('native/tests/')):
                for literal in set(_LITERAL.findall(self.text[file])):
                    literal = literal.replace('\\\\', '/')
                    if '/' not in literal:
                        if literal:
                            names.setdefault(literal, set()).add(file)
                            singles.setdefault(literal, set()).add(file)
                        continue
                    parts = [p for p in literal.split('/') if p not in ('', '.')]
                    folder = literal.endswith('/')
                    if parts and '%' in parts[-1]:  # ("type/%1": the name is put in later)
                        parts, folder = parts[:-1], True
                    if not parts or '..' in parts:
                        continue
                    if len(parts) == 1:
                        singles.setdefault(parts[0], set()).add(file)   # ("pcraft/", "/plugins/": a folder by its name)
                        continue
                    paths.setdefault(parts[0], []).append((parts, folder, file))
                    ending.setdefault(parts[-1], set()).add(file)
            self._literals = (names, singles, paths, ending)
        return self._literals

    def data_roots(self):
        """試験がマクロで受け取る木の場所（GENKO_TEST_DATA など）: 名前では呼ばれないフォルダー。"""
        suite = self.cmake.get('native/tests/CMakeLists.txt', '')
        roots = {''}
        for tail in re.findall(r'\b\w+=\\?"\$\{GENKO_SOURCE_ROOT\}((?:/[\w.\-]+)*)\\?"', suite):
            roots.add(normal_path('native' + tail))
        return roots

    def files_naming(self, path, by_name):
        """path を、二つ以上の部品の部分パス（途中までを含む）か、ファイル名（by_name のとき）で名指しする試験の木のソース。"""
        names, _, paths, _ = self.literal_index()
        parts = path.split('/')
        files = set(names.get(parts[-1], ())) if by_name else set()
        for start, part in enumerate(parts):
            for wanted, folder, file in paths.get(part, ()):
                k = len(wanted)
                if parts[start:start + k] == wanted:
                    files.add(file)
                elif not folder and len(parts) >= start + k and parts[start:start + k - 1] == wanted[:-1] and parts[start + k - 1].startswith(wanted[-1]):
                    files.add(file)  # (a path cut inside its last part: "legacy/book-" + version)
        return files

    def tests_holding(self, files):
        result = set()
        for file in files:
            reach = self.reaching(file)
            result |= {name for name, sources in self.tests.items() if reach & set(sources)}
        return result

    def tests_naming(self, path, by_name=True):
        """path を名指しする文字列を持つソースを閉包に持つ試験（足すだけの用途: ソースを文字として読む試験、埋め込みデータを木から読む試験）。"""
        return self.tests_holding(self.files_naming(path, by_name))

    def data_readers(self, path, by_name=True):
        """データのファイルを読む試験と、それで全部だと言えるか。返すのは (試験, 言えるか)。

        読むとみなすのは、名指し（files_naming）と、すぐ上のフォルダーの名前だけの文字列（"demo4"、"pcraft/"）を持つソース
        （部品から組み立てたパス: root / "fixtures" / "studio" / "demo4" / name）。すぐ上のフォルダーの名前で終わる部分パス
        （その先を実行時に足せる）を書いているのに path の場所には当たらないソースが一つでもあれば、誰が読むか言い切れない
        （全体へ）。フォルダーの先まで書いて別のファイルを指している文字列（"…/demo4/bible.json"）は、path を読まない。
        """
        _, singles, _, ending = self.literal_index()
        files = self.files_naming(path, by_name)
        folder = path.rsplit('/', 1)[0] if '/' in path else ''
        sure = True
        if folder not in self.data_roots():
            parent = folder.rsplit('/', 1)[-1]
            files |= singles.get(parent, set())
            sure = ending.get(parent, set()) <= files
        return self.tests_holding(files), sure

    def document_readers(self, path):
        """文書を読んで比べる試験: 二つ以上の部品の部分パスで名指しするか、ファイル名と、すぐ上のフォルダーの名前の両方を書いているソース。"""
        names, singles, _, _ = self.literal_index()
        files = self.files_naming(path, by_name=False)
        if '/' in path:
            folder, name = path.rsplit('/', 1)
            files |= names.get(name, set()) & singles.get(folder.rsplit('/', 1)[-1], set())
        return self.tests_holding(files)


_TREES = {}


def source_tree(root):
    """root の SourceTree。木（ファイルの名前・大きさ・時刻）が同じあいだは、同じプロセスの中で読み直さない。"""
    root = Path(root).resolve()
    seen = []
    for top in CODE_ROOTS + ('native/resources',):
        for folder, folders, names in os.walk(root / top):
            folders.sort()
            for name in sorted(names):
                status = os.stat(os.path.join(folder, name))
                seen.append((folder, name, status.st_size, status.st_mtime_ns))
    signature = hash(tuple(seen))
    cached = _TREES.get(str(root))
    if cached is None or cached[0] != signature:
        cached = _TREES[str(root)] = (signature, SourceTree(root))
    return cached[1]


TEST_STATEMENTS = ('target_compile_definitions', 'target_link_libraries', 'add_dependencies', 'set_tests_properties', 'target_sources', 'target_compile_options')


def suite_statements(code, names, kept):
    """native/tests/CMakeLists.txt を、試験ごとの文と、それ以外（共通の部分）に分ける。

    共通の部分には、試験の文があった場所に印（⟦試験名⟧）を残すので、文の位置と入れ子（if・foreach の中か外か）も比べられる。
    印に書くのは kept の試験だけ（新しく足された試験の文は、どこに置いても共通の部分を変えない）。続けて並ぶ同じ印は一つにまとめる
    （既にある試験の文のすぐ後ろに、その試験の文を足しても、共通の部分は変わらない）。
    """
    own = {}
    rest = []
    last = 0
    for command, body, start, end in cmake_commands(code):
        words = body.split()
        targets = []
        if command == 'genko_test' and words:
            targets = words[:1]
        elif command in TEST_STATEMENTS and words:
            # (set_tests_properties(a b PROPERTIES …): every test named before the first keyword)
            for word in words:
                if word not in names:
                    break
                targets.append(word)
        elif command == 'set_property' and len(words) > 1 and words[0] == 'TEST':
            for word in words[1:]:
                if word not in names:
                    break
                targets.append(word)
        if not targets or start < last:
            continue
        rest.append(code[last:start])
        marked = sorted(t for t in targets if t in kept)
        if marked:
            rest.append(' ⟦' + ','.join(marked) + '⟧ ')
        last = end
        for target in targets:
            own.setdefault(target, []).append(command + '(' + ' '.join(words) + ')')
    rest.append(code[last:])
    text = ' '.join(''.join(rest).split())
    text = re.sub(r'(⟦[^⟧]*⟧)(?: \1)+', r'\1', text)
    return own, text


def changed_tests_of_suite(before, after):
    """試験の登録ファイルの前後を比べる。試験ごとの文だけが、その場で変わったなら、その試験の集合。

    共通の部分（補助・定義・関数・if・foreach）が変わった、試験の文が別の場所・別の入れ子へ動いた、登録が消えた試験がある、
    のどれかなら None（全体へ）。
    """
    def names(code):
        return {body.split()[0] for command, body, _, _ in cmake_commands(code) if command == 'genko_test' and body.split()}
    old, new = cmake_code(before.replace('\r\n', '\n')), cmake_code(after.replace('\r\n', '\n'))
    old_names, new_names = names(old), names(new)
    if old_names - new_names:
        return None  # (a test whose registration is gone)
    kept = old_names & new_names
    old_own, old_rest = suite_statements(old, old_names | new_names, kept)
    new_own, new_rest = suite_statements(new, old_names | new_names, kept)
    if old_rest != new_rest:
        return None
    return {name for name in new_own if old_own.get(name) != new_own[name]}


_SOURCE_WORD = re.compile(r'(?<![\w./${}"\-])[A-Za-z_][\w/]*\.(?:cpp|c|cc|cxx)(?![\w."}])')


def library_sources(text):
    """ライブラリのCMakeから、ソースの並び（文ごと）と、それ以外の文字を取り出す。

    並びと認めるのは genko_library(名前 SOURCES …) の SOURCES の所と、set(…_SOURCES …) だけ。ほかの文に出るソースの名前
    （set_source_files_properties・add_executable など）は「それ以外」に残る（変われば全体へ）。
    """
    code = cmake_code(text.replace('\r\n', '\n'))
    lists = {}
    rest = []
    last = 0
    for command, body, start, end in cmake_commands(code):
        words = body.split()
        if not words or start < last:
            continue
        if command == 'genko_library' and 'SOURCES' in words:
            head = body.index('SOURCES') + len('SOURCES')
            stop = re.compile(r'\b(?:PUBLIC_DEPS|PRIVATE_DEPS)\b').search(body, head)
            tail = stop.start() if stop else len(body)
        elif command == 'set' and words[0].endswith('_SOURCES'):
            head = body.index(words[0]) + len(words[0])
            tail = len(body)
        else:
            continue
        key = command + ' ' + words[0]
        if key in lists:
            return None, None   # (two lists of one name: not read)
        found = set()

        def take(match):
            found.add(match.group(0))
            return ' '
        middle = _SOURCE_WORD.sub(take, body[head:tail])
        lists[key] = found
        rest.append(code[last:start] + command + '(' + body[:head] + middle + body[tail:] + ')')
        last = end
    rest.append(code[last:])
    return lists, ' '.join(''.join(rest).split())


def changed_sources_of_library(before, after):
    """ライブラリのCMakeの前後を比べる。変わったのがソースの並びの中だけなら、足された・外された名前。

    並びの外が変わった（コンパイルの指定にソースを足した、など）、並びが増減した、ソースが並びの間を動いた、のどれかなら None。
    """
    old_lists, old_rest = library_sources(before)
    new_lists, new_rest = library_sources(after)
    if old_lists is None or new_lists is None or old_rest != new_rest or set(old_lists) != set(new_lists):
        return None
    added = {(key, name) for key, found in new_lists.items() for name in found - old_lists[key]}
    removed = {(key, name) for key, found in old_lists.items() for name in found - new_lists[key]}
    if {name for _, name in added} & {name for _, name in removed}:
        return None  # (a source moved from one library's list to another's)
    return {name for _, name in added | removed}


BUILD_CONTROL = ('native/cmake/', 'native/vendor/', 'native/third_party/', 'native/packaging/')
BUILD_FILES = ('native/CMakeLists.txt', 'native/CMakePresets.json', 'native/vcpkg.json', '.gitattributes')
SHARED_REASON = '共通モデル・保存・権限・oracle境界'
NAME_RULE = '名前の規則（2026-11-13 までのまま）: '


def name_rule(path, inv):
    """2026-11-13 までの名前の規則が当たるパスなら (試験の集合, 理由)。当たらなければ None。

    render の、名前に系統のあるファイル（raster_ops・selection、tone・effect・ruler、3d・mannequin）と native/src/app/。
    この規則は前から受け入れられている（工程出口では Release が全試験を流す）。閉包は、フォルダーごとに全体へ送っていた
    変更にだけ使う（閉包で置き換えると、ここは前より多く選ぶことになる）。
    """
    if path.endswith('.md'):
        return None
    all_tests = inv['normal'] | inv['contract']
    if path.startswith('native/src/app/'):
        return {n for n in all_tests if n.startswith('test_gui_') or n in ('test_session', 'test_ops_e2e')}, NAME_RULE + 'app は test_gui_*・test_session・test_ops_e2e'
    if path.startswith('native/src/render/'):
        name = PurePosixPath(path).stem
        family = {n for n in inv['normal'] if any(k in n for k in ('image', 'png', 'render', 'stroke', 'raster', 'ops', 'pynum', 'pyrandom'))}
        if name in ('raster_ops', 'selection'):
            # (the selection's and the masks' tests too: they draw through raster_ops and selection)
            return family | {n for n in all_tests if any(k in n for k in ('raster', 'select', 'mask'))}, NAME_RULE + 'render の raster_ops・selection'
        if any(k in name for k in ('tone', 'effect', 'ruler')):
            return family | {n for n in all_tests if any(k in n for k in ('tone', 'ruler'))}, NAME_RULE + 'render の tone・effect・ruler'
        if any(k in name for k in ('3d', 'mannequin')):
            return family | {n for n in all_tests if '3d' in n}, NAME_RULE + 'render の 3d・mannequin'
    return None


def built_in(path, source):
    """ビルドが埋め込むデータ（CMake の雛形・qrc）を使うソースの閉包の試験。返すのは (試験 または None, 理由)。

    None で理由があれば、埋め込み方を読み取れない（全体へ）。None で理由が空なら、ビルドは path を埋め込んでいない。
    """
    tests = set()
    explained = False
    templates = {path} if path.endswith('.in') else set()
    found, unreadable = (set(), False) if templates else source.embedded(path)
    if unreadable:
        return None, 'CMake が使うデータ（使い方を読み取れない）'
    for template in sorted(templates | found):
        consumers = source.consumers_of_template(template)
        if not consumers:
            return None, '埋め込みデータ（使うソースを読み取れない）'
        explained = True
        for consumer in sorted(consumers):
            tests |= source.tests_reaching(consumer)
    for name, (qrc, listed) in sorted(source.resources().items()):
        if path == qrc or path in listed:
            users = source.resource_users(name)
            if not users:
                return None, 'Qt の resource（読み込むソースを読み取れない）'
            explained = True
            for user in sorted(users):
                tests |= source.tests_reaching(user)
    return (tests, '') if explained else (None, '')


def classify(path, tree, inv, before=None):
    """一つの変更パスの扱い。返すのは (種類, 関係する試験, 理由)。種類は skip / infrastructure / broad / related。

    tree は SourceTree を返す関数（要るときだけ木を読む）。before(path) は基点でのファイルの文字（無ければ None）。
    """
    all_tests = inv['normal'] | inv['contract']
    known = all_tests | inv['perf']

    def related(tests, reason):
        # (a path that reaches every test stays "related" here: make_plan widens the plan for it, and knows it was
        # classified — which a path the rules could not read is not)
        return 'related', set(tests) & known, reason

    if path.startswith('docs/') or path.endswith('.md'):
        # 文書はどこにあっても先に見る（2026-11-13 までと同じ順）。ビルドが埋め込む手引きはその閉包、試験が読んで比べる
        # 文書はその試験、ほかは何も流さない。文書の変更で全体へは広げない。
        source = tree()
        if not (source.root / path).is_file():
            return 'skip', set(), ''
        tests = set()
        if path.startswith(('native/src/', 'native/resources/')):
            tests, reason = built_in(path, source)
            if tests is None and reason:
                return 'broad', set(), reason
            tests = tests or set()
            if tests:
                return related(tests | source.document_readers(path), '埋め込みデータ・resource: 読むソースの閉包')
        tests = source.document_readers(path)
        return related(tests, '試験が名指しで読む文書') if tests else ('skip', set(), '')
    if path in ('.github/workflows/native.yml', '.github/disabled-workflows/native.yml') or path.startswith('tools/ci/'):
        return 'infrastructure', set(), 'CI自身の変更: 実ビルド・非契約全試験・両OSで確認'
    if path.startswith(BUILD_CONTROL) or path in BUILD_FILES:
        return 'broad', set(), 'ビルド・依存・配布・登録の共通境界'
    if path.startswith(('tools/migration/', 'tools/acceptance/')):
        return 'broad', set(), SHARED_REASON
    named = name_rule(path, inv)
    if named:
        return 'related', named[0], named[1]
    if path == 'native/tests/CMakeLists.txt':
        old = before(path) if before else None
        if old is None or path not in tree().cmake:
            return 'broad', set(), 'ビルド・依存・配布・登録の共通境界'
        changed = changed_tests_of_suite(old, tree().read(path))
        if changed is None:
            return 'broad', set(), 'ビルド・依存・配布・登録の共通境界'
        return related(changed, '試験の登録: 文の変わった試験だけ')
    if path.startswith('native/src/') and path.endswith('/CMakeLists.txt'):
        old = before(path) if before else None
        if old is None or path not in tree().cmake:
            return 'broad', set(), 'ビルド・依存・配布・登録の共通境界'
        changed = changed_sources_of_library(old, tree().read(path))
        if changed is None:
            return 'broad', set(), 'ビルド・依存・配布・登録の共通境界'
        folder = path.rsplit('/', 1)[0]
        tests = set()
        for name in sorted(changed):
            source = normal_path(folder + '/' + name)
            if source not in tree().text:
                continue  # (a source taken out of the build: the files that used it are in the diff themselves)
            if tree().unexplained(source):
                return 'broad', set(), '閉包で説明できないソース（どのヘッダーも連れて来ない）'
            tests |= tree().tests_reaching(source)
        return related(tests, 'ライブラリのソースの並びだけの変更: 足されたソースの閉包')
    if path.startswith(CODE_ROOTS) and path.endswith(CODE_SUFFIXES):
        source = tree()
        if path not in source.text:
            return 'broad', set(), '消えた・名前の変わったソース（今の木から閉包を作れない）'
        if source.unexplained(path):
            return 'broad', set(), '閉包で説明できないソース（どのヘッダーも連れて来ない）'
        # (and the tests that read the sources as text: "/native/src/" — a partial path of two parts or more)
        tests = source.tests_reaching(path) | source.tests_naming(path, by_name=False)
        if not tests:
            return 'broad', set(), 'どの試験の閉包にも入らないソース'
        return related(tests, 'ソースの閉包')
    if path.startswith('native/') and not path.startswith(('native/src/', 'native/tests/', 'native/resources/')):
        return 'broad', set(), '未分類native変更を安全側で全体へ拡大'
    if path.startswith(('native/src/', 'native/resources/')):
        # ビルドが埋め込むデータ: 埋め込み方（CMake の雛形・qrc）を読み取れたものだけ。試験が木から直接読む分も足す。
        source = tree()
        if path not in source.present and not (source.root / path).is_file():
            return 'broad', set(), '消えた・名前の変わったデータ'
        tests, reason = built_in(path, source)
        if tests is None:
            return 'broad', set(), reason or '未分類native変更を安全側で全体へ拡大'
        return related(tests | source.tests_naming(path), '埋め込みデータ・resource: 読むソースの閉包')
    if path.startswith('native/tests/'):
        source = tree()
        tests, sure = source.data_readers(path) if path in source.present else (set(), False)
        if not tests or not sure:
            return 'broad', set(), '試験の木のデータ（読む試験を言い切れない）'
        return related(tests, '試験のデータ: 名指しで読む試験')
    # native の外の、文書でないもの: 試験が部分パスで名指しして読み、ほかに読む所が無いと言えるものだけ関係づける。
    tests, sure = tree().data_readers(path, by_name=False) if (tree().root / path).is_file() else (set(), False)
    if tests and sure:
        return related(tests, '試験が名指しで読むファイル')
    return 'broad', set(), '未分類変更'


class TreeIsNotTheHead(Exception):
    pass


def make_plan(paths, phase, root, gpu=False, before=None, all_tests=False, tree_is_head=True):
    """paths は基点からの変更パス。before(path) は基点でのファイルの文字を返す関数（無ければ、CMake の変更は全体へ）。

    all_tests: 分類の結果によらず、計画の全構成で全通常試験にする（分類を当てにできないとき。構成は増やさない）。
    tree_is_head: False なら、今の木は差分の head ではない。木を読んで決める分類（閉包・データ）は使わず全体へ送る。
    """
    if phase not in PHASES:
        raise ValueError('不明な検証段階')
    paths = sorted(set(normal_path(p) or p for p in paths))
    inv = inventory(root)
    widen = all_tests
    all_tests = inv['normal'] | inv['contract']
    related = set(SAFETY)
    reasons = []
    classification = {}
    broad = not paths
    if broad:
        reasons.append('差分なし・基点不明: 全体へ拡大')
    if widen:
        broad = True
        reasons.append('指定（--all-tests）: 分類によらず全体へ拡大')
    # 分類できなかった変更があるか（差分なし・--all-tests・全体へ送ったパス・CI 自身の変更）。写真加工の試験を ASan から
    # 外すのは、これが無いときだけ。「共有のコードが全試験に届く」は分類できた結果なので、ここには数えない。
    unsure = ['差分なし' if not paths else '--all-tests'] if broad else []
    infrastructure = False
    native_change = False
    shared = set()
    narrowed = False
    holder = []

    def tree():
        if not tree_is_head:
            raise TreeIsNotTheHead
        if not holder:
            holder.append(source_tree(root))
        return holder[0]

    for path in paths:
        try:
            kind, tests, reason = classify(path, tree, inv, before)
        except TreeIsNotTheHead:
            # (as before the closure: a document starts nothing, a test's own source is that test, anything else is everything)
            stem = PurePosixPath(path).stem
            if path.startswith('docs/') or path.endswith('.md'):
                kind, tests, reason = 'skip', set(), ''
            elif path.startswith('native/tests/') and not path.startswith('native/tests/support/') and stem in all_tests | inv['perf']:
                kind, tests, reason = 'related', {stem}, '試験自身のソース（名前から。--head が checkout と違う）'
                if stem in inv['perf']:
                    reasons.append('性能試験変更: 別の性能gateを必須')
            else:
                kind, tests, reason = 'broad', set(), '--head が checkout している版と違う（閉包は今の木を読む）: 全体へ拡大'
        classification[path] = kind + ('' if kind in ('skip', 'infrastructure') else f': {reason}' + (f'（{len(tests)} 試験）' if kind == 'related' else ''))
        if kind == 'skip':
            continue
        if kind == 'infrastructure':
            infrastructure = True
            reasons.append(reason)
            unsure.append(path)
            continue
        native_change = True
        if kind == 'broad':
            broad = True
            reasons.append(reason)
            unsure.append(path)
            continue
        related |= tests & all_tests
        if all_tests <= tests:
            broad = True
            reasons.append(SHARED_REASON if path.startswith(('native/src/core/', 'native/src/storage/', 'native/tests/support/'))
                           else '共有描画経路' if path.startswith('native/src/render/') else reason + '（全試験に届く）')
        # (a path the policy of 2026-11-13 sent to every test — now decided by the closure —, or one it had a rule for:
        # the rules by names, a test's own source)
        if not (reason.startswith(NAME_RULE) or path.startswith('native/tests/') and not path.startswith('native/tests/support/')
                and PurePosixPath(path).stem in all_tests | inv['perf']):
            narrowed = True
        if path.startswith(('native/src/core/', 'native/src/storage/', 'native/tests/support/')):
            shared.add(SHARED_REASON)
        elif path.startswith('native/src/render/'):
            shared.add('共有描画経路')
        if path.startswith('native/tests/') and reason == 'ソースの閉包' and any(path in tree().tests.get(name, ()) for name in inv['perf']):
            reasons.append('性能試験変更: 別の性能gateを必須')
    if not broad and native_change and all_tests <= related:
        broad = True
        reasons.append('関係する試験が全試験に及ぶ')
        reasons += sorted(shared)
    exit_phase = phase in ('milestone', 'final')
    if phase == 'final':
        broad = True
        native_change = True
        reasons.append('最終配布候補: 全通常試験を全構成（Debug を含む）で実行')
    elif phase == 'milestone':
        native_change = True
        reasons.append('工程出口: linux-release は全通常試験、linux-asan と Windows は差分に関係する試験（全体へ拡大した差分は全試験）。Debug は最終配布の前に一度（--phase final）')
    rows = []
    touched_photo = [p for p in paths if p.startswith(PHOTO_PATHS)]
    without_photo = []
    if broad or native_change or infrastructure:
        if broad:
            tier = 'full'
            presets = ['linux-release', 'linux-asan', 'windows-debug', 'windows-release']
            if phase == 'final':
                presets.insert(0, 'linux-debug')
        elif phase == 'milestone':
            tier = 'milestone'
            presets = ['linux-release', 'linux-asan', 'windows-debug', 'windows-release']
        elif infrastructure:
            tier = 'infrastructure' if not native_change else 'related'
            presets = ['linux-release', 'linux-asan', 'windows-debug', 'windows-release']
            # (with a CI change the plan keeps the four builds; Debug is added for a development diff the rules by names
            # decide, as before — not for one the closure narrowed from "every test", which never had it)
            if native_change and phase == 'development' and not narrowed:
                presets.insert(0, 'linux-debug')
        else:
            tier = 'related' if phase == 'development' else 'integration'
            presets = ['linux-debug', 'linux-asan', 'windows-debug'] if phase == 'development' else ['linux-release', 'linux-asan', 'windows-debug', 'windows-release']
        for preset in presets:
            full = broad or (phase in ('integration', 'milestone') and preset == 'linux-release' and native_change)
            selected = set(all_tests if full else (inv['normal'] | (related if native_change else set())) if infrastructure else related)
            # WindowsにPython oracleは従来どおり置かない。未実施を成功と呼ばない。
            excluded = sorted(selected & inv['contract']) if preset.startswith('windows') else []
            if preset.startswith('windows'):
                selected -= inv['contract']
            selected &= all_tests
            # (PC5 H-a: the graphics card's way on Xvfb, asked for, on Linux, with the app's tests)
            if gpu and preset.startswith('linux') and (full or any(n.startswith('test_gui_') for n in selected)):
                selected |= inv['gpu']
            if preset == 'linux-asan' and phase != 'final' and not unsure and not touched_photo:
                # 利用者の決定 2026-11-14: 写真加工のコードに触れていない差分では、写真加工の試験を ASan にかけない。
                without_photo = sorted(selected & PHOTO_TESTS)
                selected -= PHOTO_TESTS
            if not selected:
                raise ValueError('native試験の選択が空です')
            why = ('全通常試験: ' + ('最終配布候補' if phase == 'final' else '差分を全体へ拡大' if broad else '工程出口の安全網' if phase == 'milestone' else '統合候補の全契約'
                   ) if full else 'CI自身の変更: 非契約全試験と関係する契約' if infrastructure else '差分に関係する試験と常時保護の対照')
            reasons.append(f'{preset}: {why}（{len(selected)} 試験' + ('、写真加工の試験を除く' if preset == 'linux-asan' and without_photo else '') + '）')
            rows.append({'preset': preset, 'suite': 'full' if full else 'related',
                         'tests': sorted(selected), 'excluded_contracts': excluded})
        if without_photo:
            reasons.append(f'写真加工の試験 {len(without_photo)} 本は ASan から外した（利用者の決定 2026-11-14: 写真加工のコードに触れていない。Release では流す）')
        elif phase == 'final':
            reasons.append('写真加工の試験は ASan でも流す: 最終配布候補')
        elif touched_photo:
            reasons.append('写真加工の試験は ASan に残した: 写真加工のコードに触れている（' + '、'.join(touched_photo[:3]) + (f' ほか {len(touched_photo) - 3}' if len(touched_photo) > 3 else '') + '）')
        elif unsure:
            reasons.append('写真加工の試験は ASan に残した: 分類できない変更がある（' + '、'.join(unsure[:3]) + (f' ほか {len(unsure) - 3}' if len(unsure) > 3 else '') + '）')
    else:
        tier = 'documents'
        reasons.append('文書だけの変更: nativeビルドなし')
    return {'schema': 2, 'phase': phase, 'tier': tier, 'paths': paths,
            'reasons': sorted(set(reasons)), 'classification': classification, 'asan_without_photo': without_photo,
            'matrix': {'include': rows},
            'has_native': bool(rows), 'performance_separate': True,
            'performance_required': exit_phase or any('/perf/' in p for p in paths)}


def choose_baseline(runs, branch, is_ancestor):
    for row in runs:
        sha = row.get('head_sha', '')
        if row.get('head_branch') == branch and row.get('event') == 'push' and row.get('status') == 'completed' and row.get('conclusion') == 'success' and re.fullmatch(r'[0-9a-f]{40}', sha) and is_ancestor(sha):
            return row
    return None


# 再利用するPRは工程出口の全構成だけ。通常PRの関連試験は基点にしない。
REUSE_CONTROLS = ('.github/workflows/native.yml', 'tools/ci', 'native/cmake',
                  'native/CMakeLists.txt', 'native/CMakePresets.json', 'native/vcpkg.json')


def choose_milestone_baseline(runs, is_ancestor, same_controls) -> dict[str, Any] | None:
    for row in runs:
        sha = row.get('head_sha', '')
        if (row.get('event') == 'pull_request' and row.get('status') == 'completed'
                and row.get('conclusion') == 'success'
                and row.get('head_branch', '').startswith('native/milestone/')
                and re.fullmatch(r'[0-9a-f]{40}', sha)
                and is_ancestor(sha) and same_controls(sha)):
            return dict(row, _verified_full_milestone=True)
    return None


def last_successful_push(root, repository, branch, head):
    # 公開リポジトリのmetadataだけ。認証/permission追加なし。取得不能は全体へ。
    import urllib.parse
    import urllib.request
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('repository識別子が不正')
    resolved = subprocess.check_output(['git', 'rev-parse', '--verify', '--end-of-options', head + '^{commit}'], cwd=root).decode('ascii').strip()
    def fetch(event, event_branch=None):
        query = {'event': event, 'status': 'success', 'per_page': 20}
        if event_branch is not None:
            query['branch'] = event_branch
        url = f'https://api.github.com/repos/{repository}/actions/workflows/native.yml/runs?' + urllib.parse.urlencode(query)
        with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'genko-validation-policy'}), timeout=15) as response:
            return json.load(response)['workflow_runs']
    def ancestor(sha):
        return subprocess.run(['git', 'merge-base', '--is-ancestor', sha, resolved], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0
    def controls(sha):
        return subprocess.run(['git', 'diff', '--quiet', sha, resolved, '--', *REUSE_CONTROLS], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0
    push = choose_baseline(fetch('push', branch), branch, ancestor)
    try:
        full_pr = choose_milestone_baseline(fetch('pull_request'), ancestor, controls)
    except (OSError, ValueError, KeyError):
        # 照合できるpush基点があればそれを使い、照合不能なPRを合格扱いにしない。
        return push
    if full_pr and (not push or subprocess.run(['git', 'merge-base', '--is-ancestor', push['head_sha'], full_pr['head_sha']], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0):
        return full_pr
    return push


def head_is_checked_out(root, head):
    """閉包は今の木のソースを読む。head が checkout している版でなければ、木を読む分類は使わない。"""
    def commit(ref):
        done = subprocess.run(['git', 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}'], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return done.stdout.decode('ascii', 'replace').strip() if done.returncode == 0 else None
    return commit(head) is not None and commit(head) == commit('HEAD')


def changed_paths(root, base, head):
    # diff失敗は例外で停止。差分なしはmake_plan側で全体へ拡大する。
    for ref in (base, head):
        subprocess.run(['git', 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}'], cwd=root, check=True, stdout=subprocess.PIPE)
    # (--no-renames: a renamed file is its old path gone and its new path added; the old one is not hidden)
    return subprocess.check_output(['git', 'diff', '--name-only', '--no-renames', '-z', base, head, '--'], cwd=root).decode('utf-8').split('\0')[:-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True)
    parser.add_argument('--head', default='HEAD')
    parser.add_argument('--phase', choices=PHASES, default='development')
    parser.add_argument('--output', required=True)
    parser.add_argument('--github-output')
    parser.add_argument('--ci-baseline-branch')
    parser.add_argument('--repository')
    # (PC5 H-a: the GUI tests on Xvfb and Mesa too — test_gui_canvas_gpu —; the machine needs xvfb-run and Mesa)
    parser.add_argument('--gpu', action='store_true')
    # 分類を当てにできないとき（validation_closure_check.py が不合格のあいだ）: 計画の全構成で全通常試験。構成は増やさない。
    parser.add_argument('--all-tests', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    base = args.base
    evidence = None
    baseline_error = None
    if args.ci_baseline_branch:
        if not args.repository:
            parser.error('--repositoryが必要です')
        try:
            evidence = last_successful_push(root, args.repository, args.ci_baseline_branch, args.head)
            base = evidence['head_sha'] if evidence else None
        except (OSError, ValueError, KeyError) as error:
            base = None
            baseline_error = str(error)
    paths = changed_paths(root, base, args.head) if base else []
    if (evidence and evidence.get('event') == 'pull_request'
            and evidence.get('_verified_full_milestone') and not paths
            and args.phase == 'integration'):
        plan = {'schema': 1, 'phase': args.phase, 'paths': [], 'tier': 'reused-full-milestone',
                'has_native': False, 'matrix': {'include': []},
                'performance_required': False,
                'reasons': ['全構成合格PRと全tree・検証制御が同一。証跡を再使用し再buildしない']}
    else:
        def before(path):
            # (the file as it was at the base: None when it was not there)
            shown = subprocess.run(['git', 'show', f'{base}:{path}'], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            return shown.stdout.decode('utf-8', 'replace') if shown.returncode == 0 else None
        plan = make_plan(paths, args.phase, root, gpu=args.gpu, before=before if base else None,
                         all_tests=args.all_tests, tree_is_head=head_is_checked_out(root, args.head))
    plan.update(base=base, head=args.head, baseline_run=evidence.get('html_url') if evidence else None,
                baseline_error=baseline_error)
    if args.ci_baseline_branch and not base:
        plan['reasons'].append('前回合格を照合できないため全体へ拡大')
    Path(args.output).write_text(json.dumps(plan, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.github_output:
        with open(args.github_output, 'a', encoding='utf-8') as out:
            out.write('matrix=' + json.dumps(plan['matrix']) + '\n')
            out.write('has_native=' + str(plan['has_native']).lower() + '\n')
            for platform in ('linux', 'windows'):
                matrix = {'include': [r for r in plan['matrix']['include'] if r['preset'].startswith(platform)]}
                out.write(platform + '_matrix=' + json.dumps(matrix) + '\n')
    print(json.dumps(plan, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
