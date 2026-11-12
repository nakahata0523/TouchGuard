# Touchpad filter (C)

Linux evdev/uinput 版。Python 版のオプションと接触判定方式を維持し、Python/evdev パッケージへの依存をなくしています。

ビルド:

```sh
cc -O2 -std=c11 -Wall -Wextra -Wpedantic touchpad_filter.c -lm -o touchpad_filter
```

使用例（デバイスのパスは実際のタッチパッドに置き換えてください）:

```sh
sudo ./touchpad_filter /dev/input/eventN
```

/dev/input/eventN の読み取りと /dev/uinput の書き込み権限が必要です。
開始時には指を離してください。終了は Ctrl+C または SIGTERM。
Python 版と同時起動しないでください。
引数一覧は `./touchpad_filter --help`。

最適化: 最大128イベントのまとめ読み、フレーム/保留イベントのまとめ書き、
再利用する連続バッファ、スロット配列による接触管理、距離の二乗での比較。
既定値は `--threshold-ms 30 --min-move-mm 0.3 --quiet` です。
イベントログは既定で省略し、`--verbose` で表示できます。
タップクリックの押下時間20msと、その間の入力処理待ちは元版と同じです。
保留/フレームバッファは各最大1048576イベントとし、上限超過やSYN_DROPPEDでは終了します。
数値オプションは有限の正数に限定しています。
仮想デバイスの複製対象は EV_KEY / EV_ABS / EV_REL / EV_SW です。

疑似イベントテスト:

```sh
cc -O2 -std=c11 -Wall -Wextra -Wpedantic touchpad_filter_test.c -lm -o /tmp/touchpad_filter_test
/tmp/touchpad_filter_test
```

短時間接触の破棄、1本指移動の許可と保留解除、直後のタップクリック、
クリック有効期限、複数指操作を検証します。
実際の evdev/uinput デバイスでの動作およびCPU使用量は別途実機で確認が必要です。
