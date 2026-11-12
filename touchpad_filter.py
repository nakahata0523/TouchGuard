#!/usr/bin/env python3

import argparse
import math
import sys
import time

from evdev import InputDevice, UInput, ecodes as e


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument("device")
    parser.add_argument("--threshold-ms", type=float, default=100)
    parser.add_argument("--min-move-mm", type=float, default=1.0)
    parser.add_argument("--click-window-ms", type=float, default=2000)
    parser.add_argument("--tap-max-ms", type=float, default=250)
    parser.add_argument("--tap-slop-mm", type=float, default=1.5)
    parser.add_argument("--motion-step-mm", type=float, default=0.1)

    args = parser.parse_args()

    if min(args.threshold_ms, args.min_move_mm,
           args.tap-max-ms if False else args.tap_max_ms,
           args.tap_slop_mm, args.motion_step_mm) <= 0:
        parser.error("Thresholds must be positive")

    threshold = args.threshold_ms / 1000
    click_window = args.click_window_ms / 1000
    tap_max = args.tap_max_ms / 1000

    dev = InputDevice(args.device)
    ui = None
    click_ui = None
    grabbed = False

    try:
        caps = dev.capabilities(absinfo=True)

        abs_info = {
            code: info for code, info in caps.get(e.EV_ABS, [])
        }

        key_codes = set(caps.get(e.EV_KEY, []))

        required = {
            e.ABS_MT_SLOT,
            e.ABS_MT_TRACKING_ID,
            e.ABS_X,
            e.ABS_Y,
        }

        if not required.issubset(abs_info):
            raise RuntimeError("Unsupported touchpad")

        if e.BTN_LEFT not in key_codes:
            raise RuntimeError("BTN_LEFT is unavailable")

        if e.BTN_TOUCH in dev.active_keys():
            raise RuntimeError("Lift fingers before starting")

        xres = abs_info[e.ABS_X].resolution
        yres = abs_info[e.ABS_Y].resolution

        if not xres or not yres:
            raise RuntimeError("Invalid coordinate resolution")

        ignored_buttons = {
            code for code in (e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE)
            if code in key_codes
        }

        current_slot = dev.absinfo(e.ABS_MT_SLOT).value
        active_slots = set()

        pointer_x = dev.absinfo(e.ABS_X).value
        pointer_y = dev.absinfo(e.ABS_Y).value

        # 接触セッション
        session_start = None
        session_start_pos = None
        session_max_fingers = 0
        session_max_distance = 0.0
        session_accepted = False
        pending_frames = []

        # 最後に許可された1本指の移動時刻
        last_move_time = None

        def distance_mm(a, b):
            dx = (a[0] - b[0]) / xres
            dy = (a[1] - b[1]) / yres
            return math.hypot(dx, dy)

        def filter_buttons(events):
            return [
                ev for ev in events
                if not (
                    ev.type == e.EV_KEY
                    and ev.code in ignored_buttons
                )
            ]

        def emit_frame(events):
            wrote = False

            for ev in events:
                if ev.type in (e.EV_SYN, e.EV_MSC):
                    continue

                if ev.type == e.EV_KEY and ev.code in ignored_buttons:
                    continue

                ui.write(ev.type, ev.code, ev.value)
                wrote = True

            if wrote:
                ui.syn()

        def emit_frames(frames):
            for frame in frames:
                emit_frame(frame)

        # タップをBTN_LEFTクリックに変換
        def synthesize_click():
            click_ui.write(e.EV_KEY, e.BTN_LEFT, 1)
            click_ui.syn()

            time.sleep(0.02)

            click_ui.write(e.EV_KEY, e.BTN_LEFT, 0)
            click_ui.syn()

            print("TAP CLICK ALLOW", flush=True)

        # 仮想入力デバイス
        ui = UInput.from_device(
            dev,
            filtered_types=(e.EV_SYN, e.EV_MSC),
            name=f"{dev.name} (filtered touchpad)",
            input_props=dev.input_props(),
        )

        # クリック専用の仮想マウス
        click_ui = UInput(
            {
                e.EV_KEY: [e.BTN_LEFT],
                e.EV_REL: [e.REL_X, e.REL_Y],
            },
            name="Touchpad Filter Click Mouse",
        )

        # 物理デバイスを独占
        dev.grab()
        grabbed = True

        print("Touchpad filter started")
        print("Threshold:", args.threshold_ms, "ms")
        print("Movement:", args.min_move_mm, "mm")
        print("Click window:", args.click_window_ms, "ms")
        print("Tap max:", args.tap_max_ms, "ms")

        frame = []

        for ev in dev.read_loop():

            if ev.type == e.EV_SYN and ev.code == e.SYN_DROPPED:
                raise RuntimeError("SYN_DROPPED")

            frame.append(ev)

            if not (
                ev.type == e.EV_SYN
                and ev.code == e.SYN_REPORT
            ):
                continue

            now = time.monotonic()

            previous_slots = set(active_slots)
            previous_pointer = (pointer_x, pointer_y)
            pointer_updated = False

            # 入力状態を更新
            for item in frame:

                if item.type != e.EV_ABS:
                    continue

                if item.code == e.ABS_MT_SLOT:
                    current_slot = item.value

                elif item.code == e.ABS_MT_TRACKING_ID:
                    if item.value < 0:
                        active_slots.discard(current_slot)
                    else:
                        active_slots.add(current_slot)

                elif item.code == e.ABS_X:
                    pointer_x = item.value
                    pointer_updated = True

                elif item.code == e.ABS_Y:
                    pointer_y = item.value
                    pointer_updated = True

            current_pointer = (pointer_x, pointer_y)

            was_touching = bool(previous_slots)
            touching = bool(active_slots)

            started = not was_touching and touching
            ended = was_touching and not touching

            filtered_frame = filter_buttons(frame)

            # 新しい接触
            if started:

                session_start = now
                session_start_pos = current_pointer
                session_max_fingers = len(active_slots)
                session_max_distance = 0.0
                session_accepted = False

                pending_frames = [filtered_frame]

            elif session_start is not None:

                session_max_fingers = max(
                    session_max_fingers,
                    len(active_slots),
                    len(previous_slots),
                )

                if session_max_fingers == 1:
                    session_max_distance = max(
                        session_max_distance,
                        distance_mm(
                            current_pointer,
                            session_start_pos,
                        ),
                    )

                if session_accepted:
                    emit_frame(filtered_frame)
                else:
                    pending_frames.append(filtered_frame)

            else:
                emit_frame(filtered_frame)

            # 接触時間・移動距離による判定
            if session_start is not None:

                duration = now - session_start

                if not session_accepted and duration >= threshold:

                    one_finger_move = (
                        session_max_fingers == 1
                        and session_max_distance >= args.min_move_mm
                    )

                    multitouch_gesture = session_max_fingers >= 2

                    if one_finger_move or multitouch_gesture:

                        session_accepted = True

                        emit_frames(pending_frames)
                        pending_frames = []

                        if one_finger_move:
                            last_move_time = now
                            print("POINTER MOVE ALLOW", flush=True)
                        else:
                            print("MULTITOUCH ALLOW", flush=True)

                elif session_accepted and touching:

                    # 許可済みの1本指移動
                    if (
                        len(previous_slots) == 1
                        and len(active_slots) == 1
                        and pointer_updated
                        and distance_mm(
                            current_pointer,
                            previous_pointer,
                        ) >= args.motion_step_mm
                    ):
                        last_move_time = now

            # 指が離れたとき
            if ended and session_start is not None:

                duration = now - session_start

                recent_move = (
                    last_move_time is not None
                    and 0 <= now - last_move_time <= click_window
                )

                is_tap = (
                    not session_accepted
                    and session_max_fingers == 1
                    and duration <= tap_max
                    and session_max_distance <= args.tap_slop_mm
                )

                if is_tap and recent_move:

                    # 接触イベントは破棄し、クリックを生成
                    synthesize_click()

                elif not session_accepted:

                    print(
                        f"TOUCH DROP: {duration * 1000:.0f} ms, "
                        f"{session_max_distance:.2f} mm",
                        flush=True,
                    )

                # セッションをリセット
                session_start = None
                session_start_pos = None
                session_max_fingers = 0
                session_max_distance = 0.0
                session_accepted = False
                pending_frames = []

            frame = []

    except KeyboardInterrupt:
        print("\nStopping filter")

    finally:
        if grabbed:
            try:
                dev.ungrab()
            except OSError:
                pass

        if ui is not None:
            ui.close()

        if click_ui is not None:
            click_ui.close()

        dev.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
