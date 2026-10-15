#!/usr/bin/env python3
"""Fault-injection integration test against local OBS and the dedicated audit collection.

Requires a BETTERFACE_TEST_FAULTS build and a running loopback-only RTMP stream.
Never run against a user's live collection. Results/screenshots go to the supplied directory.
"""
import base64
import io
import json
from pathlib import Path
import sys
import time

import numpy as np
from PIL import Image
from obsctl import client

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
c = client()
def req(name, **args):
    return c.send(name, args, raw=True)

assert req('GetSceneCollectionList')['currentSceneCollectionName'] == 'BetterFace Safety Test'
assert req('GetCurrentProgramScene')['currentProgramSceneName'] == 'Stability Test'
assert req('GetStreamServiceSettings')['streamServiceSettings']['server'] == 'rtmp://127.0.0.1:19358/live'
assert req('GetStreamStatus')['outputActive']
assert not req('GetRecordStatus')['outputActive']
log = max((Path.home()/'Library/Application Support/obs-studio/logs').glob('*.txt'), key=lambda p:p.stat().st_mtime)
assert '[BetterFace] loaded, version 2.0.5-test' in log.read_text()
source, scene, name = 'Audit Portrait', 'Stability Test', 'Safety Injection Test'
req('SetSourceFilterEnabled', sourceName=source, filterName='BetterFace Audit', filterEnabled=False)

def media(action):
    req('TriggerMediaInputAction', inputName=source, mediaAction='OBS_WEBSOCKET_MEDIA_INPUT_ACTION_'+action)

def shot(label):
    s = req('GetSourceScreenshot', sourceName=scene, imageFormat='png', imageWidth=540, imageHeight=960)
    data = base64.b64decode(s['imageData'].split(',', 1)[1])
    (out/(label+'.png')).write_bytes(data)
    return np.asarray(Image.open(io.BytesIO(data)).convert('RGB'))

def set_enabled(target, enabled):
    req('SetSourceFilterEnabled', sourceName=target, filterName=name, filterEnabled=enabled)
    time.sleep(.15)

cases = [(p, source) for p in ['conversion', 'input', 'vb_create', 'vb_data', 'mask',
         'geom_pass', 'seg_texture', 'warp_texture', 'BlurH', 'Down', 'Smooth',
         'technique', 'pass_begin', 'Final']]
cases += [(p, scene) for p in ['stage_create', 'stage_map', 'Draw']]
with (out/'fault-results.jsonl').open('x', buffering=1) as report:
    for point, target in cases:
        row = {'point': point, 'target': target, 'start': time.time()}
        mark = len(log.read_text())
        before = req('GetStreamStatus')
        media('RESTART')
        time.sleep(.4)
        req('CreateSourceFilter', sourceName=target, filterName=name, filterKind='betterface_filter',
            filterSettings={'smooth':60, 'slim':30, 'whiten':10, 'rosy':20, 'lift':50,
                            'neck':True, 'protect':True, 'debug':False, '_test_fault':point})
        try:
            deadline = time.monotonic()+15
            while '[BetterFace] safety bypass:' not in log.read_text()[mark:]:
                assert time.monotonic()<deadline, f'{point}: failure was not triggered'
                time.sleep(.2)
            # Even a saved or API-supplied true must be normalized back to false.
            assert req('GetSourceFilter', sourceName=target, filterName=name)['filterSettings']['protect'] is False
            media('PAUSE')
            # FFmpeg/OBS may still drain queued 59.94 fps frames after acknowledging pause.
            # Wait for identical pictures; never loosen the raw-pixel assertion to mask motion.
            time.sleep(1)
            failed = shot(point+'_failed')
            for _ in range(10):
                time.sleep(.3)
                settled = shot(point+'_settled')
                if np.array_equal(failed, settled):
                    break
                failed = settled
            else:
                raise AssertionError(f'{point}: media did not settle after pause')
            row['paused_media'] = req('GetMediaInputStatus', inputName=source)
            set_enabled(target, False)
            raw = shot(point+'_raw')
            assert np.array_equal(failed, raw), f'{point}: failed output differs from original'
            assert float(raw.std())>5, f'{point}: image is blank'
            # Remove the simulated fault and toggle the filter: the bypass must stay latched.
            req('SetSourceFilterSettings', sourceName=target, filterName=name,
                filterSettings={'_test_fault':'', 'protect':True})
            set_enabled(target, True)
            latched = shot(point+'_latched')
            assert np.array_equal(latched, raw), f'{point}: settings/toggle reset the bypass'
            media('PLAY')
            media('PLAY')
            time.sleep(.5)
            moving1 = shot(point+'_moving1')
            time.sleep(2.6)
            moving2 = shot(point+'_moving2')
            assert not np.array_equal(moving1, moving2), f'{point}: picture stopped advancing'
            after = req('GetStreamStatus')
            assert after['outputActive'] and not after['outputReconnecting']
            assert after['outputTotalFrames']>before['outputTotalFrames']+60
            assert after['outputSkippedFrames']==before['outputSkippedFrames']
            lines = [l for l in log.read_text()[mark:].splitlines() if '[BetterFace] safety bypass:' in l]
            assert len(lines)==1, f'{point}: repeated failure logging/retries'
            row.update(passed=True, raw_equal=True, latch_preserved=True, moving=True,
                       output_frames=after['outputTotalFrames']-before['outputTotalFrames'], message=lines[0])
        except Exception as e:
            row.update(passed=False, error=str(e))
            raise
        finally:
            row['end']=time.time()
            report.write(json.dumps(row)+'\n')
            print(json.dumps(row), flush=True)
            req('RemoveSourceFilter', sourceName=target, filterName=name)
            media('PLAY')
    req('SetSourceFilterEnabled', sourceName=source, filterName='BetterFace Audit', filterEnabled=True)
print(json.dumps({'completed':True,'cases':len(cases)}), flush=True)
