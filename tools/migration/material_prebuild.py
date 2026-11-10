#!/usr/bin/env python3
"""組込み素材のJSONと56pxプレビューを開発時に固定。製品実行時にPythonを要求しない。"""
from pathlib import Path
import argparse, hashlib, json, random


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output
    if output.exists():
        raise ValueError('既存証跡/配布素材を上書きしません。新規出力先を指定してください')
    from genko import materials, models, render
    entries = materials.load_catalog()
    if len({entry['id'] for entry in entries}) != len(entries):
        raise ValueError('組込み素材IDが重複しています')
    output.mkdir(parents=True)
    original_id = models.new_id
    rows = []
    manifest = {'scope': '組込み素材プレビューのみ。素材配置/ユーザー素材/全描画機能の受入ではない',
                'size': 56, 'files': {}, 'source': {}}
    source_root = Path(materials.__file__).resolve().parents[1]
    for source in sorted(source_root.rglob('*.py')):
        manifest['source'][str(source.relative_to(source_root))] = hashlib.sha256(source.read_bytes()).hexdigest()
    manifest['source']['materials/catalog.json'] = hashlib.sha256((source_root/'materials/catalog.json').read_bytes()).hexdigest()
    try:
        for entry in entries:
            digest = hashlib.sha256(entry['id'].encode()).hexdigest()
            counter = 0
            def new_id():
                nonlocal counter
                counter += 1
                return hashlib.sha256(f'{digest}:{counter}'.encode()).hexdigest()[:16]
            models.new_id = new_id
            random.seed(digest)
            render._STROKE_CACHE.clear()
            image = materials.thumbnail(entry, 56).convert('RGB')
            if image.size != (56, 56):
                raise ValueError(f"プレビュー寸法が不正: {entry['id']}")
            name = digest + '.png'
            image.save(output/name, format='PNG')
            manifest['files'][name] = hashlib.sha256((output/name).read_bytes()).hexdigest()
            rows.append({**entry, 'preview': ':/genko/materials/' + name})
    finally:
        models.new_id = original_id
        render._STROKE_CACHE.clear()
    catalog = json.dumps(rows, ensure_ascii=False, indent=2) + '\n'
    (output/'catalog.json').write_text(catalog, encoding='utf-8')
    manifest['files']['catalog.json'] = hashlib.sha256((output/'catalog.json').read_bytes()).hexdigest()
    manifest['count'] = len(rows)
    (output/'MANIFEST.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'count':len(rows),'files':len(manifest['files']),'output':str(output)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
