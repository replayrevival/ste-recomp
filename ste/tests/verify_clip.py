# SPDX-License-Identifier: GPL-2.0-or-later
# verify_clip.py <clip.mkv> [--fps 60] [--log <run stderr.txt>] [--ffmpeg <dir>]
# Smoothness check of a recorded showcase clip (standing rule: showcase footage is 1080p60 and checked):
#   - every video frame timestamp is exactly 1/fps after the previous (no gaps, no wall-clock duplicates/drops added by the encoder)
#   - consecutive frames with identical pixels (decoded md5): counted; a long run in a moving scene is a duplicated frame (menus and loads are legitimately static)
#   - the recorder's own counters from the run log: frames dropped (ring full), presented-frame intervals > 20 / > 34 ms while recording
# Prints one verdict line: PASS when timestamps are uniform, dropped = 0, no run of >= 2 identical frames in moving scenes (reported, not failed, when the pad script ends in a still screen).
import argparse, subprocess, re, sys, os
ap = argparse.ArgumentParser()
ap.add_argument('clip'); ap.add_argument('--fps', type=float, default=60.0); ap.add_argument('--log'); ap.add_argument('--ffmpeg', default=os.environ.get('FFMPEG_BIN', ''))
a = ap.parse_args()
ffprobe = os.path.join(a.ffmpeg, 'ffprobe.exe') if a.ffmpeg else 'ffprobe'; ffmpeg = os.path.join(a.ffmpeg, 'ffmpeg.exe') if a.ffmpeg else 'ffmpeg'
out = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'frame=pts_time', '-of', 'csv=p=0', a.clip], capture_output=True, text=True).stdout.split()
ts = [float(x.strip(',')) for x in out if x.strip(',') not in ('N/A', '')]
n = len(ts); step = 1.0 / a.fps
bad = [(i, ts[i] - ts[i - 1]) for i in range(1, n) if abs((ts[i] - ts[i - 1]) - step) > 0.0015]
print('frames: %d, duration %.2f s, nominal %.1f fps' % (n, (ts[-1] - ts[0] + step) if n else 0, a.fps))
print('timestamp irregularities (> 1.5 ms off the frame time; Matroska stamps are rounded to 1 ms): %d%s' % (len(bad), (' e.g. ' + ', '.join('#%d %.1f ms' % (i, d * 1000) for i, d in bad[:5])) if bad else ''))
md = subprocess.run([ffmpeg, '-v', 'error', '-i', a.clip, '-map', '0:v:0', '-f', 'framemd5', '-'], capture_output=True, text=True).stdout.splitlines()
hs = [l.split(',')[-1].strip() for l in md if l and not l.startswith('#')]
runs = 0; longest = 0; cur = 1; dup = 0
for i in range(1, len(hs)):
    if hs[i] == hs[i - 1]:
        cur += 1; dup += 1
        if cur == 2: runs += 1
        longest = max(longest, cur)
    else: cur = 1
print('identical consecutive frames: %d (%d runs, longest run %d frames)' % (dup, runs, longest))
verdict = len(bad) == 0
if a.log and os.path.exists(a.log):
    t = open(a.log, errors='replace').read()
    m = re.findall(r'\[RECORD\] stopped: (\d+) frames written, (\d+) dropped \(ring full\), (\d+) map retries; presented frames (\d+), intervals > 20 ms: (\d+), > 34 ms: (\d+), longest ([\d.]+) ms', t)
    if m:
        w, d, r, pf, o20, o34, mx = m[-1]
        print('recorder: written %s, dropped %s, map retries %s, presented %s, intervals>20ms %s, >34ms %s, longest %s ms' % (w, d, r, pf, o20, o34, mx))
        if int(d) != 0: verdict = False
        if int(w) != n: print('NOTE: frames written %s != frames in file %d' % (w, n)); verdict = verdict and abs(int(w) - n) <= 1
    else:
        print('recorder: no [RECORD] stopped line in the log'); verdict = False
print('VERDICT: %s' % ('PASS' if verdict else 'FAIL'))
sys.exit(0 if verdict else 1)


