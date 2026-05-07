# Simulated unit test for FluidNCClient::handleIncoming()
# Runs on host Python and mimics parsing outputs from the firmware.
import re
import time

def now_ms():
    return int(time.time() * 1000)

class Status:
    def __init__(self):
        self.is_connected = False
        self.state = 'DISCONNECTED'
        self.mpos = (0.0,0.0,0.0)
        self.wco = (0.0,0.0,0.0)
        self.wpos = (0.0,0.0,0.0)
        self.feed_rate = 0.0
        self.spindle_speed = 0.0
        self.feed_override = 0.0
        self.rapid_override = 0.0
        self.spindle_override = 0.0
        self.is_sd_printing = False
        self.sd_percent = 0.0
        self.sd_filename = ''
        self.sd_start_time_ms = 0
        self.sd_elapsed_ms = 0
        self.last_message = ''

S = Status()
_last_status_log = 0
_ever_connected = False


def handle_incoming(payload):
    global _last_status_log, _ever_connected
    if not payload or payload.strip() == '':
        return
    payload = payload.strip()
    if not payload.startswith('<'):
        print(f"[FluidNC] Received: {payload}")
    # Terminal/message callbacks would be called here in firmware
    if payload.startswith('<'):
        parse_status_report(payload)
    elif payload.startswith('[GC:'):
        print(f"[FluidNC] GCode State: {payload}")
    elif payload.startswith('['):
        parse_realtime_feedback(payload)


def parse_status_report(message):
    global _last_status_log, _ever_connected
    now = now_ms()
    # Auto-report confirmation
    # Mark connected
    if not S.is_connected:
        S.is_connected = True
        print('[FluidNC] ✓ Connection established (status received)')
        _ever_connected = True
    # Throttle logging to 5s like firmware
    if now - _last_status_log >= 5000:
        print(f"[FluidNC] Status update (5s): {message}")
        _last_status_log = now
    # State detection
    if '<Idle' in message: S.state = 'IDLE'
    elif '<Run' in message: S.state = 'RUN'
    elif '<Hold' in message: S.state = 'HOLD'
    elif '<Jog' in message: S.state = 'JOG'
    elif '<Alarm' in message: S.state = 'ALARM'
    # MPos
    m = re.search(r'MPos:([\d\.-]+),([\d\.-]+),([\d\.-]+)', message)
    if m:
        S.mpos = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
    # WCO
    m = re.search(r'WCO:([\d\.-]+),([\d\.-]+),([\d\.-]+)', message)
    if m:
        S.wco = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
        print(f"[FluidNC] WCO updated: ({S.wco[0]:.3f},{S.wco[1]:.3f},{S.wco[2]:.3f})")
    # WPos
    m = re.search(r'WPos:([\d\.-]+),([\d\.-]+),([\d\.-]+)', message)
    if m:
        S.wpos = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
    else:
        S.wpos = (S.mpos[0]-S.wco[0], S.mpos[1]-S.wco[1], S.mpos[2]-S.wco[2])
    # FS
    m = re.search(r'FS:([\d\.-]+),([\d\.-]+)', message)
    if m:
        S.feed_rate = float(m.group(1))
        S.spindle_speed = float(m.group(2))
        print(f"[FluidNC] Parsed FS: feed={S.feed_rate:.0f}, spindle={S.spindle_speed:.0f}")
    # Ov
    m = re.search(r'Ov:([\d\.-]+),([\d\.-]+),([\d\.-]+)', message)
    if m:
        S.feed_override = float(m.group(1))
        S.rapid_override = float(m.group(2))
        S.spindle_override = float(m.group(3))
        print(f"[FluidNC] Parsed Ov: feed={S.feed_override:.0f}%%, rapid={S.rapid_override:.0f}%%, spindle={S.spindle_override:.0f}%%")
    # SD
    m = re.search(r'SD:([\d\.-]+),([^|>]+)', message)
    if m:
        percent = float(m.group(1))
        filename = m.group(2).strip()
        if S.sd_start_time_ms == 0:
            S.sd_start_time_ms = now_ms()
        S.sd_elapsed_ms = now_ms() - S.sd_start_time_ms
        S.is_sd_printing = True
        S.sd_percent = percent
        S.sd_filename = filename
        print(f"[FluidNC] SD Progress: {percent:.1f}% - {filename} (Elapsed: {S.sd_elapsed_ms}ms)")
    else:
        if S.is_sd_printing:
            print('[FluidNC] SD file completed or stopped')
        S.is_sd_printing = False
        S.sd_percent = 0
        S.sd_start_time_ms = 0
        S.sd_elapsed_ms = 0
        S.sd_filename = ''
    print(f"[FluidNC] Status: State={S.state}, MPos=({S.mpos[0]:.3f},{S.mpos[1]:.3f},{S.mpos[2]:.3f}), WPos=({S.wpos[0]:.3f},{S.wpos[1]:.3f},{S.wpos[2]:.3f})")


def parse_realtime_feedback(message):
    print(f"[FluidNC] Feedback: {message}")
    if message.startswith('[PRB:'):
        m = re.match(r'\[PRB:([\d\.-]+),([\d\.-]+),([\d\.-]+):([01])\]', message)
        if m:
            x,y,z,ok = float(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4))
            print(f"[FluidNC] Probe {'SUCCESS' if ok else 'FAILED'} at ({x:.3f}, {y:.3f}, {z:.3f})")
    if 'websocket auto report interval set' in message:
        print('[FluidNC] ✓ Auto-report confirmed - automatic reporting enabled')


if __name__ == '__main__':
    tests = [
        # Simple status
        '<Idle|MPos:0.000,0.000,0.000|FS:0,0|Ov:100,100,100>',
        # Status with WCO
        '<Idle|MPos:10.500,5.250,-1.234|FS:120.0,1500|WCO:0.500,0.250,1.000|Ov:90,100,100>',
        # SD progress
        '<Run|MPos:50.000,25.000,-5.000|FS:300.0,0|SD:12.5,example.gcode>',
        # Probe feedback
        '[PRB:151.000,149.000,-137.505:1]',
        # Auto-report confirmation
        '[MSG:websocket auto report interval set to 250ms]'
    ]

    for t in tests:
        print('\n--- Feeding payload:')
        print(t)
        handle_incoming(t)
        time.sleep(0.2)

    print('\nSimulation complete.')
