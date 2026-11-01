#!/usr/bin/env python3
"""中文：只发布 S31 的已校验二进制；源码仍在私有工程。 / English: Stage verified S31 binaries only; its firmware sources remain private."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import struct
from release_assets import _hydrate_files


def stage_metalio(flash: Path, output: Path) -> None:
    source=flash/'metalio'
    manifest=json.loads((source/'manifest.json').read_text())
    assets=json.loads((source/'release-assets.json').read_text())
    version=manifest['version']
    assert re.fullmatch(r'\d+\.\d+\.\d+-rc\d+',version)
    assert assets['schema']==1 and assets['tag']=='metalio-s31-v'+version
    assert manifest['board']=='metalio47' and manifest['project']=='Kiikoread_Metalio_S31'
    assert manifest['new_install_prompt_erase'] is True
    assert manifest['pico_ota']['layout']=='metalio-s31-dual-5m-v1'
    builds=manifest['builds']
    assert len(builds)==1 and builds[0]['chipFamily']=='ESP32-S31'
    expected={'bootloader.bin':0x2000,'partitions.bin':0x8000,'firmware.bin':0x10000,'ota_data_initial.bin':0xd000}
    assert len(builds[0]['parts'])==4 and set(assets['files'])==set(expected)
    assert {p['path']:p['offset'] for p in builds[0]['parts']}==expected
    for part in builds[0]['parts']:
        info=assets['files'][part['path']]
        assert part['size']==info['size'] and part['sha256']==info['sha256']
    destination=output/'metalio';destination.mkdir(parents=True,exist_ok=True)
    _hydrate_files(source,destination,assets,maximum=0x500000)
    for name in ('manifest.json','releases.json','index.html'):
        shutil.copy2(source/name,destination/name)
    if (source/'README.txt').is_file():shutil.copy2(source/'README.txt',destination/'README.txt')
    app=(destination/'firmware.bin').read_bytes()
    assert 288<=len(app)<=0x500000 and app[0]==0xe9 and struct.unpack_from('<H',app,12)[0]==32
    assert struct.unpack_from('<I',app,32)[0]==0xabcd5432
    assert app[48:80].split(b'\0')[0].decode()==version
    assert app[80:112].split(b'\0')[0]==b'Kiikoread_Metalio_S31'
    boot=(destination/'bootloader.bin').read_bytes()
    assert 0<len(boot)<=0x6000 and boot[0]==0xe9 and struct.unpack_from('<H',boot,12)[0]==32
    table=(destination/'partitions.bin').read_bytes()
    expected_parts=[('nvs',1,2,0x9000,0x4000),('otadata',1,0,0xd000,0x2000),('phy_init',1,1,0xf000,0x1000),('ota_0',0,16,0x10000,0x500000),('ota_1',0,17,0x510000,0x500000),('storage',1,0x81,0xa10000,0x5e0000),('coredump',1,3,0xff0000,0x10000)]
    entries=[]
    for at in range(0,len(table),32):
        magic=struct.unpack_from('<H',table,at)[0]
        if magic==0xebeb:
            assert table[at+16:at+32]==hashlib.md5(table[:at]).digest()
            assert table[at+32:]==b'\xff'*(len(table)-at-32)
            break
        assert magic==0x50aa
        _,kind,sub,address,size,label,flags=struct.unpack_from('<HBBII16sI',table,at)
        assert flags==0
        entries.append((label.split(b'\0')[0].decode(),kind,sub,address,size))
    else:raise AssertionError('Missing partition checksum')
    assert entries==expected_parts
    fixed=f'Metalio-S31-update-{version}.bin'
    shutil.copy2(destination/'firmware.bin',output/fixed)
    shutil.copy2(destination/'firmware.bin',output/'Metalio-S31-update.bin')
    feed=json.loads((flash/'metalio-s31-update.json').read_text())
    assert feed['schema']==1 and feed['version']==version and feed['project']==manifest['project']
    assert feed['board']=='Metalio-EINK4-Plus-S31' and feed['layout']=='metalio-s31-dual-5m-v1'
    assert feed['minimum_base_version']=='0.3.3-rc98'
    assert feed['size']==len(app) and feed['sha256']==hashlib.sha256(app).hexdigest()
    assert feed['url']=='https://wegooo-cell.github.io/read-pico-reader/'+fixed
    assert isinstance(feed['notes'],str) and 0<len(feed['notes'].encode())<384
    shutil.copy2(flash/'metalio-s31-update.json',output/'metalio-s31-update.json')
    # 历史版本必须来自原 Release，所有旧网址保留原字节。 / Historical URLs retain original Release bytes.
    history={}
    for release in assets.get('historical',[]):
        assert re.fullmatch(r'metalio-s31-v\d+\.\d+\.\d+-rc\d+',release['tag'])
        fixed='Metalio-S31-update-'+release['tag'].removeprefix('metalio-s31-v')+'.bin'
        assert set(release['files'])=={fixed}
        _hydrate_files(flash,output,release,maximum=0x500000)
        history[release['tag'].removeprefix('metalio-s31-v')]=release['files'][fixed]
    catalog=json.loads((source/'releases.json').read_text())
    for entry in catalog['releases']:
        old=entry['version'];assert old in history and old!=version
        assert entry['manifest']==f'metalio/releases/{old}/manifest.json' and entry['bin']==f'Metalio-S31-update-{old}.bin'
        archive=source/'releases'/old;old_manifest=json.loads((archive/'manifest.json').read_text())
        assert old_manifest['version']==old and old_manifest['board']=='metalio47' and old_manifest['builds'][0]['chipFamily']=='ESP32-S31'
        target=output/'metalio/releases'/old;target.mkdir(parents=True,exist_ok=True)
        shutil.copy2(archive/'manifest.json',target/'manifest.json')
        payload=(output/entry['bin']).read_bytes()
        assert payload[48:80].split(b'\0')[0].decode()==old and payload[80:112].split(b'\0')[0]==b'Kiikoread_Metalio_S31'
        for name in ('bootloader.bin','partitions.bin','ota_data_initial.bin'):
            data=(archive/name).read_bytes();info=entry['files'][name]
            assert len(data)==info['size'] and hashlib.sha256(data).hexdigest()==info['sha256']
            shutil.copy2(archive/name,target/name)
    previous=manifest.get('previous_version')
    if previous:assert any(entry['version']==previous for entry in catalog['releases'])
    print(f'Metalio S31: {version}, {len(app)} bytes, independent 5 MiB slots, verified SHA-256')
