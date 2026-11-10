#!/usr/bin/env python3
"""PhotoCraft最初の縦断単位: 正式CLI・disk Undo・GC・関連回帰。V2全体ではない。"""
from pathlib import Path
import hashlib
import json
import os
import re
import struct
import subprocess
from PIL import Image

ROOT = Path('/src')
BUILD = ROOT / 'build/photocraft-v1'
OUT = BUILD / 'regression-v6'
PRIOR = BUILD / 'regression-v5'
OUT.mkdir(exist_ok=True)
G = BUILD / 'src/api/genko'
ACTOR = 'human:カラー受入'
result = {'status': 'running', 'scope': 'PhotoCraftカラー/露光量 V1 Linux Debug 関連回帰のみ',
          'commands': [], 'checks': []}

def pins():
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((ROOT / 'native').rglob('*')) if p.is_file()}

def cgroup():
    return {p: (Path('/sys/fs/cgroup') / p).read_text().strip()
            for p in ('cpu.max', 'memory.max', 'memory.events', 'memory.peak')}

def save():
    (OUT / 'execution.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')

def run(name, args, expected=0, env=None):
    actual_env = dict(os.environ, QT_QPA_PLATFORM='xcb', GENKO_CONFIG_DIR=str(OUT/'config'),
                      GENKO_COLOR_SCREENSHOT_DIR=str(OUT/'gui-visual'))
    if env:
        actual_env.update(env)
    process = subprocess.run([str(x) for x in args], cwd=ROOT, env=actual_env,
                             capture_output=True, timeout=600)
    (OUT/(name+'.stdout')).write_bytes(process.stdout)
    (OUT/(name+'.stderr')).write_bytes(process.stderr)
    result['commands'].append({'name': name, 'argv': [str(x) for x in args],
                               'exit': process.returncode, 'expected_exit': expected})
    save()
    assert process.returncode == expected, (name, process.returncode, process.stdout[-4000:], process.stderr[-4000:])
    return process

def cli(name, args, expected=0):
    return json.loads(run(name, [G, *args], expected).stdout)

def apply(name, book, ops, expected=0):
    path = OUT/(name+'.ops.json')
    path.write_text(json.dumps(ops, ensure_ascii=False))
    return cli(name, ['apply', book, path, '--agent', ACTOR], expected)

def payload(book):
    return json.loads((book/'project.json').read_text())

def asset(book, meta):
    ref = meta['asset'].removeprefix('sha256:')
    return book/'assets'/ref[:2]/(ref+'.colorrgba')

def all_files(book):
    files = {str(p.relative_to(book)): p.read_bytes() for p in book.rglob('*') if p.is_file()}
    lock = json.loads(files['project.lock'])
    assert lock['released'] is True and lock['agent'] == ACTOR, lock
    del lock['released_at']  # normal release timestamp only; ownership/released remain checked
    files['project.lock'] = json.dumps(lock, sort_keys=True).encode()
    return files

result['source_before'] = pins()
result['driver_pins_before'] = {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
    for p in [Path(__file__).resolve(),ROOT/'tools/acceptance/photocraft_growth_probe.c']}
cache = (BUILD/'CMakeCache.txt').read_text()
result['configuration'] = dict(re.findall(r'^([A-Z_][A-Z_0-9]*):[^=\r\n]+=([^\r\n]*)$',cache,re.M))
result['cgroup_before'] = cgroup()
result['binary_before'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
    for p in [G, *(BUILD/'tests'/n for n in ('test_color_raster','test_storage','test_command_bus',
                     'test_render_page','test_session','test_journal','test_gui_color','test_gui_actions'))]}
save()
try:
    assert result['configuration']['CMAKE_BUILD_TYPE'] == 'Debug'
    assert result['configuration']['GENKO_WERROR'] == 'ON'
    assert result['configuration']['GENKO_FAULT_INJECTION'] == 'ON'
    assert result['cgroup_before']['cpu.max'].split() == ['100000','100000']
    assert result['cgroup_before']['memory.max'] == '2147483648'
    for name in ('test_color_raster','test_storage','test_command_bus','test_render_page',
                 'test_session','test_journal','test_gui_color','test_gui_actions'):
        log = OUT/(name+'.txt')
        previous = json.loads((PRIOR/'execution.json').read_text())
        reusable = (previous['source_before'] == previous['source_after'] == result['source_before']
                    and previous['binary_before'] == previous['binary_after'] == result['binary_before']
                    and previous['oom_kill_delta'] == 0
                    and previous['cgroup_before']['cpu.max'] == result['cgroup_before']['cpu.max']
                    and previous['cgroup_before']['memory.max'] == result['cgroup_before']['memory.max'])
        prior_commands = [x for x in previous['commands'] if x['name'] == name]
        if reusable and len(prior_commands) == 1 and prior_commands[0]['exit'] == 0:
            log.write_bytes((PRIOR/(name+'.txt')).read_bytes())
            result['commands'].append({'name':name,'exit':0,'reused_from':str(PRIOR/'execution.json')})
        else:
            run(name, [BUILD/'tests'/name, '-o', str(log)+',txt'])
        raw = log.read_bytes()
        totals = re.findall(rb'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', raw)
        assert len(totals) == 1, (name, 'missing totals')
        passed, failed, skipped = map(int, totals[0])
        assert passed > 0 and failed == 0 and skipped == 0, (name, totals)
        result['checks'].append({'name': name, 'passed': passed, 'failed': failed, 'skipped': skipped})
        save()

    book = OUT/'rgba16.genko'
    cli('new16', ['new', book, '--title','正式カラー受入','--pages','2','--webtoon','--json'])
    # Known unported nombre: disable only in a synthetic input, before the first human save.
    seed = payload(book)
    seed['pages'][0]['numero'] = False
    (book/'project.json').write_text(json.dumps(seed,ensure_ascii=False))
    result['fixture_edits'] = ['合成入力のnumero=false。set_nombre/ノンブルの受入ではない']
    assert apply('put16', book, [{'op':'put_color_raster','page':1,'width':1,'height':1,
                               'precision':'u16','pixels':[1,2,3,65535]}])['ok']
    original = payload(book)
    meta = original['pages'][0]['layers'][-1]['color_raster']
    source_path = asset(book, meta)
    source_bytes = source_path.read_bytes()
    assert struct.unpack_from('<4H', source_bytes, 16) == (1,2,3,65535)
    assert hashlib.sha256(source_bytes).hexdigest() == meta['asset'].removeprefix('sha256:')
    assert apply('exposure16', book, [{'op':'add_layer','page':1,'layer':'finish','kind':'adjust',
                         'adjust':{'kind':'exposure','exposure':10}}])['ok']
    adjusted = payload(book)
    assert adjusted['pages'][0]['layers'][-2]['color_raster'] == meta
    assert 'native.color_raster_v1' in adjusted['features']
    assert 'native.exposure_v1' in adjusted['features']
    assert source_path.read_bytes() == source_bytes
    for mode in ('print','proof'):
        png = OUT/('rgba16-'+mode+'.png')
        assert cli('render16-'+mode, ['render',book,'--page','1','--dpi','72','--mode',mode,'--out',png])['ok']
    with Image.open(OUT/'rgba16-print.png') as image:
        pixel = image.convert('RGB').getpixel((image.width//2,image.height//2))
        assert pixel == (4,8,12), pixel
    assert cli('undo16', ['undo',book,'--as',ACTOR])['ok']
    undone = payload(book)
    assert undone['pages'][0]['layers'][-1]['color_raster'] == meta
    assert cli('redo16', ['redo',book,'--as',ACTOR])['ok']
    assert payload(book)['pages'][0]['layers'] == adjusted['pages'][0]['layers']
    before = all_files(book)
    rejected = apply('refuse-merge16', book, [{'op':'set_note','page':2,'note':'前置変更'},
                           {'op':'merge_down','page':1,'layer_id':meta.get('id',adjusted['pages'][0]['layers'][-2]['id'])}], 1)
    assert not rejected['ok'] and rejected['code'] == 'not_yet_ported', rejected
    assert all_files(book) == before, 'refused batch changed saved files/lock'
    assert cli('gc16', ['gc',book])['ok']
    assert source_path.read_bytes() == source_bytes
    assert cli('reload16', ['inspect',book,'--full'])['pages'][0]['layers'][-1]['kind'] == 'adjust'
    assert cli('doctor16', ['doctor',book])['ok']
    result['checks'].append({'name':'正式CLI RGBA16/露光量/feature/disk Undo-Redo/拒否時全files不変/GC/再読込','passed':True})
    save()

    hdr = OUT/'rgba32.genko'
    cli('new32', ['new',hdr,'--title','HDR受入','--pages','2','--webtoon','--json'])
    seed = payload(hdr)
    seed['pages'][0]['numero'] = False
    (hdr/'project.json').write_text(json.dumps(seed,ensure_ascii=False))
    assert apply('put32', hdr, [{'op':'put_color_raster','page':1,'width':1,'height':1,
                         'precision':'f32','pixels':[1e10,-0.25,0.1234500035648346,1]}])['ok']
    hdr_meta = payload(hdr)['pages'][0]['layers'][-1]['color_raster']
    hdr_raw = asset(hdr,hdr_meta).read_bytes()
    assert hdr_raw[16:] == struct.pack('<4f',1e10,-0.25,0.1234500035648346,1)
    assert apply('exposure32',hdr,[{'op':'add_layer','page':1,'layer':'finish','kind':'adjust',
                                'adjust':{'kind':'exposure'}}])['ok']
    assert cli('render32',['render',hdr,'--page','1','--dpi','72','--mode','print','--out',OUT/'rgba32.png'])['ok']
    assert asset(hdr,hdr_meta).read_bytes() == hdr_raw
    with Image.open(OUT/'rgba32.png') as image:
        pixel = image.convert('RGB').getpixel((image.width//2,image.height//2))
        assert pixel == (255,0,31), pixel
    result['checks'].append({'name':'正式CLI RGBA32負値/HDR/低bit保存・露光量描画','passed':True})
    probe = OUT/'growth-probe.so'
    run('compile-growth-probe',['cc','-shared','-fPIC','-O0',ROOT/'tools/acceptance/photocraft_growth_probe.c','-o',probe,'-ldl'])
    original_bytes = source_path.read_bytes()
    try:
        proc = run('opened-asset-growth',[G,'doctor',book],expected=1,
                   env={'LD_PRELOAD':str(probe),'GENKO_PROBE_ASSET':str(source_path)})
        assert b'grew=1' in proc.stderr, proc.stderr
        assert b'asset grew during bounded read' in proc.stdout, proc.stdout
        reads = re.findall(rb'GENKO_GROWTH_READ request=(\d+) actual=(\d+)',proc.stderr)
        assert reads and sum(int(got) for _,got in reads) <= len(original_bytes)+1, reads
        result['checks'].append({'name':'実readerのopen後素材増大を注入、上限外読込・確保を拒否','passed':True,
                                 'reads':[(int(a),int(b)) for a,b in reads]})
    finally:
        source_path.write_bytes(original_bytes)
    result['status'] = 'passed'
except BaseException as error:
    result['status'] = 'failed'
    result['error'] = str(error)
finally:
    result['source_after'] = pins()
    result['driver_pins_after'] = {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [Path(__file__).resolve(),ROOT/'tools/acceptance/photocraft_growth_probe.c']}
    result['cgroup_after'] = cgroup()
    result['binary_after'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [G, *(BUILD/'tests'/n for n in ('test_color_raster','test_storage','test_command_bus',
                         'test_render_page','test_session','test_journal','test_gui_color','test_gui_actions'))]}
    before = dict(x.split() for x in result['cgroup_before']['memory.events'].splitlines())
    after = dict(x.split() for x in result['cgroup_after']['memory.events'].splitlines())
    result['oom_kill_delta'] = int(after['oom_kill'])-int(before['oom_kill'])
    if result['driver_pins_before'] != result['driver_pins_after'] or result['source_before'] != result['source_after'] or result['binary_before'] != result['binary_after'] or result['oom_kill_delta']:
        result['status'] = 'failed'
        result['pin_or_oom_failure'] = True
    save()
print(json.dumps({k:result.get(k) for k in ('status','error','checks','oom_kill_delta')},ensure_ascii=False),flush=True)
raise SystemExit(0 if result['status']=='passed' else 1)
