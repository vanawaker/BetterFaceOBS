#!/usr/bin/env python3
"""Keep pkgbuild's bundle metadata but remove generated AppleDouble payload entries.

On newer macOS, protected provenance attributes can survive xattr -c and pkgbuild
serializes them as ._ sidecars even with COPYFILE_DISABLE. Repack only when needed,
using Apple's package tools and a cpio payload with explicit metadata exclusion.
"""
from pathlib import Path
import math
import subprocess
import sys
import xml.etree.ElementTree as ET

stage, component = map(lambda p: Path(p).resolve(), sys.argv[1:])
def output(*args):
    return subprocess.check_output(args, text=True)
def run(*args):
    subprocess.run(args, check=True)
def sidecar(path):
    return any(part.startswith('._') for part in Path(path).parts)

names = output('pkgutil', '--payload-files', str(component)).splitlines()
if not any(map(sidecar, names)):
    sys.exit(0)

work = component.with_suffix('.cleanwork')
run('pkgutil', '--expand', str(component), str(work))
rows = output('lsbom', str(work/'Bom')).splitlines()
kept = [r for r in rows if not sidecar(r.split('\t')[0])]
actual = {'.'} | {'./'+str(p.relative_to(stage)) for p in stage.rglob('*')}
assert {r.split('\t')[0] for r in kept} == actual, 'Unexpected payload entry mismatch'
assert not any(map(sidecar, actual)), 'Source staging contains metadata sidecars'

listing = work.parent/(work.name+'.bom-list.txt')
listing.write_text('\n'.join(kept)+'\n')
run('mkbom', '-i', str(listing), str(work/'Bom'))
run('/usr/bin/tar', '--format=odc', '--no-mac-metadata', '--no-xattrs', '--no-acls',
    '--uid', '0', '--gid', '0', '-czf', str(work/'Payload'), '-C', str(stage), '.')
assert set(output('/usr/bin/tar', '-tf', str(work/'Payload')).splitlines()) == actual

xml = ET.parse(work/'PackageInfo')
payload = xml.getroot().find('payload')
# pkgbuild counts the payload root along with its descendants.
assert int(payload.get('numberOfFiles')) == len(rows)
payload.set('numberOfFiles', str(len(kept)))
payload.set('installKBytes', str(math.ceil(sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())/1024)))
xml.write(work/'PackageInfo', encoding='utf-8', xml_declaration=True)
saved = component.with_suffix('.with-metadata.pkg')
assert not saved.exists()
component.rename(saved)
run('pkgutil', '--flatten', str(work), str(component))
assert set(output('pkgutil', '--payload-files', str(component)).splitlines()) == actual
print('Repacked component without AppleDouble sidecars; original retained in package work directory')
