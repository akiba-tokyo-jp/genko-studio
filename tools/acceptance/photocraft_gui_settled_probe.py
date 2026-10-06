#!/usr/bin/env python3
"""Nativeソースを変えず、現在のGUIテストにタイル完了と全画素対照を追加する一時probe。"""
from pathlib import Path
import hashlib
import json
import os
import shlex
import subprocess
ROOT=Path('/src')
B=ROOT/'build/photocraft-v1'
OUT=B/'gui-settled-probe'
OUT.mkdir(exist_ok=True)
source=(ROOT/'native/tests/gui/test_gui_color.cpp').read_text()
source=source.replace('        const QString screenshots=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));\n        if (!screenshots.isEmpty()) {', '''        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.canvas()->fit_page();
        QTRY_VERIFY_WITH_TIMEOUT(window.canvas()->renderer().settled(),10000);
        const auto referenceAt = [](const app::DocPtr& referenceDoc,int dpi) {
            render::RenderOptions referenceOptions;
            referenceOptions.mode="proof";
            referenceOptions.skip_unported=true;
            const auto referenceImage=render::render_page(referenceDoc->page(0),dpi,referenceOptions,referenceDoc.get()).image;
            const auto bytes=referenceImage.tobytes();
            return QImage(reinterpret_cast<const uchar*>(bytes.data()),referenceImage.width(),referenceImage.height(),referenceImage.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
        };
        QTRY_COMPARE_WITH_TIMEOUT(window.canvas()->renderer().compose(window.canvas()->renderer().shown_dpi()),
                                 referenceAt(session->snapshot(),window.canvas()->renderer().shown_dpi()),10000);
        const QString screenshots=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty()) {''')
assert 'QTRY_COMPARE_WITH_TIMEOUT' in source
probe=OUT/'test_gui_color.cpp'
probe.write_text(source)
commands=subprocess.check_output(['ninja','-C',str(B),'-t','commands','test_gui_color'],text=True).splitlines()
compile_cmds=[x for x in commands if ' -c /src/native/tests/gui/test_gui_color.cpp' in x]
link_cmds=[x for x in commands if ' -o tests/test_gui_color ' in x]
assert len(compile_cmds)==len(link_cmds)==1,(len(compile_cmds),len(link_cmds))
obj=OUT/'test_gui_color.o'
exe=OUT/'test_gui_color'
args=shlex.split(compile_cmds[0])
args[args.index('-c')+1]=str(probe)
args[args.index('-o')+1]=str(obj)
args[args.index('-MF')+1]=str(OUT/'test_gui_color.d')
args.append('-I/src/native/tests/gui')
subprocess.run(args,cwd=B,check=True)
link=shlex.split(link_cmds[0])
# Ninja link commands use shell prefixes, not product inputs; run only the actual compiler argv.
assert link[:3]==[':', '&&','/usr/bin/c++'],link[:3]
link=link[2:]
if link[-2:]==['&&',':']: link=link[:-2]
old_obj='tests/CMakeFiles/test_gui_color.dir/gui/test_gui_color.cpp.o'
assert old_obj in link
link[link.index(old_obj)]=str(obj)
link[link.index('-o')+1]=str(exe)
subprocess.run(link,cwd=B,check=True)
env=dict(os.environ,GENKO_COLOR_SCREENSHOT_DIR=str(OUT/'visual'),QT_QPA_PLATFORM='xcb')
run=subprocess.run([str(exe),'-o',str(OUT/'qt.txt')+',txt'],env=env,cwd=ROOT,timeout=120)
(OUT/'result.json').write_text(json.dumps({'exit':run.returncode,'scope':'Qt GUI settled/all-pixel probe; product unchanged',
 'probe_sha256':hashlib.sha256(probe.read_bytes()).hexdigest(),'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()},indent=2))
raise SystemExit(run.returncode)
