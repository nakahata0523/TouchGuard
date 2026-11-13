# Touchpad Filter

## What is Touchpad Filter?

Touchpad Filterは、Linuxのタッチパッドで、カーソル移動後の一定時間だけ1本指のタップクリックを許可するC言語のプログラムです。

物理クリック、二本指クリック、スクロール、タップドラッグの認識はOSに任せます。物理ボタンや複数指の操作には、このプログラム独自の時間・移動条件を課しません。

## Setup build environment

必要な環境は次のとおりです。

| 項目 | 要件 |
|---|---|
| OS | Linux（evdev / uinput対応） |
| コンパイラ | C11対応のGCCまたはClang |
| ヘッダー | Linux input / uinputヘッダー |
| 実行権限 | 対象の`/dev/input/eventN`の読み取りと`/dev/uinput`の書き込み |

Ubuntu / Debianでは、ビルド用パッケージをインストールします。

```sh
sudo apt update
sudo apt install build-essential linux-libc-dev
```

コンパイラを確認します。

```sh
cc --version
```

`/dev/uinput`が存在しない場合は、uinputモジュールを読み込みます。

```sh
sudo modprobe uinput
ls -l /dev/uinput
```

## Build Touchpad Filter

配置済みのディレクトリに移動し、ビルドします。

```sh
cd ~/touchpad-filter
cc -O2 -std=c11 -Wall -Wextra -Wpedantic touchpad_filter.c -lm -o touchpad_filter
```

実行ファイルとオプションを確認します。

```sh
./touchpad_filter --help
```

PythonやPython用evdevパッケージのインストールは不要です。`touchpad_filter.py`は旧方式の参考用で、C版とは動作が異なります。

## Setup input device

### Find the touchpad

入力デバイスの一覧を確認します。

```sh
cat /proc/bus/input/devices
```

タッチパッドの名前がある項目を探し、`Handlers=`に記載された`eventN`を確認してください。

例えば`Handlers=mouse0 event5`なら、使用するパスは`/dev/input/event5`です。以下のコマンド例ではこの番号を使用します。自分の環境の番号に置き換えてください。

### Find a persistent device path

`eventN`の番号は再起動で変わることがあります。自動起動には、可能なら固定のリンクを使います。

```sh
ls -l /dev/input/by-path/
```

対象の`eventN`を指すリンクを探し、そのリンクの完全なパスを使用してください。`by-path`に該当するリンクがなければ、`/dev/input/by-id/`も確認します。

```sh
ls -l /dev/input/by-id/
```

`/dev/input/event5`と`/dev/input/by-path/event5`は異なります。確認したリンク名を省略したり、番号だけを`by-path`に付けたりしないでください。

### Setup desktop settings

デスクトップのタッチパッド設定で、必要に応じて「タップでクリック」「タップドラッグ」を有効にします。

このプログラムはOSの設定を変更しません。クリックの指本数、スクロール方向・速度、ドラッグの認識はOSの設定に従います。

## Launch Touchpad Filter

### Start manually

Python版やsystemdサービスが動いている場合は、先に停止してください。systemdサービスの停止方法は次のとおりです。

```sh
sudo systemctl stop touchpad-filter.service
```

タッチパッドから指を離して起動します。

```sh
cd ~/touchpad-filter
sudo ./touchpad_filter /dev/input/event5
```

既定では操作ログは出ません。ログを表示するには`--verbose`を付けます。

```sh
sudo ./touchpad_filter /dev/input/event5 --verbose
```

### Stop manually

起動した端末で **Ctrl+C** を押します。

## Setup systemd service

### Create a service file

次の例は、プログラムを`/home/nakahata/touchpad-filter/`に配置した場合の設定です。別の場所に配置している場合は実行ファイルのパスも変更してください。

```sh
sudo nano /etc/systemd/system/touchpad-filter.service
```

以下を記載します。`ExecStart`末尾の`/dev/input/event5`は例です。手動実行で確認したデバイスパスに置き換えてください。再起動後も使う場合は、前の手順で確認した固定リンクの完全なパスを推奨します。

```ini
[Unit]
Description=Touchpad input filter
StartLimitIntervalSec=0

[Service]
Type=simple
ExecStart=/home/nakahata/touchpad-filter/touchpad_filter /dev/input/event5
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

### Enable and start

手動起動したプログラムをCtrl+Cで終了し、タッチパッドから指を離して実行します。

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now touchpad-filter.service
```

### Check status and logs

```sh
systemctl status touchpad-filter.service
sudo journalctl -u touchpad-filter.service -b -n 30 --no-pager
```

システムサービスのログを確認する際は、`sudo`を付けてください。

### Restart

実行ファイルの更新後はサービスを再起動します。

```sh
sudo systemctl restart touchpad-filter.service
```

サービスファイルを編集した場合は、先に設定を再読み込みします。

```sh
sudo systemctl daemon-reload
sudo systemctl restart touchpad-filter.service
```

### Stop and disable

一時的に停止します。

```sh
sudo systemctl stop touchpad-filter.service
```

自動起動も解除する場合は、次を実行します。

```sh
sudo systemctl disable --now touchpad-filter.service
```

## Configure Touchpad Filter

### Default behavior

- 有効な移動から1.5秒以内に始まった接触は、その開始からOSへ転送します。
- 有効期間外に始まった1本指接触は一時保留し、開始位置から1.5mm動いた時点で保留分を含めて転送します。移動条件を満たさず1本指のまま離れた場合は破棄します。
- 物理ボタンや2本以上の指を検出した場合は、時間・移動条件を待たずに保留を解除します。物理ボタンは接触情報と同じ仮想タッチパッドへ転送します。
- 許可した短い接触の後は、300ms以内に始まる次の接触を転送し、OS側のタップドラッグを妨げないようにします。複数指タップ後のドラッグにも適用します。

### Options

| オプション | 既定値 | 用途 |
|---|---:|---|
| `--click-window-ms` | 1500 | 最終移動から接触開始までのタップ許可期間（ms） |
| `--min-move-mm` | 1.5 | 移動として保留を解除する距離（mm） |
| `--threshold-ms` | 30 | 有効な移動時刻を記録するために必要な接触時間（ms） |
| `--motion-step-mm` | 0.1 | 最終移動時刻を更新するためのフレーム間移動量（mm） |
| `--tap-max-ms` | 250 | ドラッグ継続用として扱う接触の最大時間（ms） |
| `--tap-slop-mm` | 1.5 | 同継続判定に使う1本指接触の最大変位（mm） |
| `--drag-window-ms` | 300 | 許可済み接触に続く次の接触を転送する期間（ms） |
| `--quiet` | 有効 | 操作ログを省略 |
| `--verbose` | 無効 | 起動・接触の許可理由・最大変位・接触時間・ボタン検出を表示 |
| `--help` | — | 使用方法を表示して終了 |

数値オプションには有限の正数を指定します。

`--threshold-ms`は物理クリックや複数指操作の待ち時間ではありません。`--motion-step-mm`も、転送中の小さな移動を捨てる設定ではありません。

タップ自体の認識条件、ボタンの割り当て、ドラッグの終了動作はOS側が決定します。`--tap-max-ms`、`--tap-slop-mm`、`--drag-window-ms`は、このプログラムが接触を継続転送するための条件です。

### Change options

例えば、タップ許可期間を3秒に変更します。

```sh
sudo ./touchpad_filter /dev/input/event5 --click-window-ms 3000
```

systemdで使用する場合は、サービスの`ExecStart`末尾にオプションを追加し、`daemon-reload`と再起動を行ってください。

## Test Touchpad Filter

疑似イベントによるテストをビルドし、実行します。実際の入力デバイスやroot権限は不要です。

```sh
cd ~/touchpad-filter
cc -O2 -std=c11 -Wall -Wextra -Wpedantic touchpad_filter_test.c -lm -o /tmp/touchpad_filter_test
/tmp/touchpad_filter_test
```

成功時は次のメッセージが表示されます。

```text
All native forwarding tests passed
```

テスト対象は、移動前の1本指タップの破棄、移動後の転送、短い複数指接触、物理ボタン・2本目の指による保留解除、ドラッグ継続、転送イベントの一致です。実機のOSでの操作感は別途確認が必要です。

## Troubleshooting

### Service keeps restarting

まずエラーログを確認します。

```sh
sudo journalctl -u touchpad-filter.service -b -n 30 --no-pager
```

`open input device`エラーの場合は、`ExecStart`のデバイスパスが存在するか確認してください。`Lift fingers before starting`の場合は指を離して次の再起動を待ちます。

`open /dev/uinput`エラーの場合は、`/dev/uinput`の存在とアクセス権を確認してください。`Unsupported touchpad`や`Invalid coordinate resolution`の場合、そのデバイスは現在の実装の対応条件を満たしていません。

### Tapping or dragging does not work

OS側の「タップでクリック」「タップドラッグ」が有効か確認してください。初回の1本指タップは、その前に有効なカーソル移動が必要です。物理クリックや複数指操作には、この移動条件はありません。

### Pressing firmly sometimes triggers a tap

押し込む際の接触位置のずれが`--min-move-mm`（既定1.5mm）を超えると、物理ボタンの入力がなくても移動として接触をOSへ転送します。OSがその接触をタップと認識するとクリックになる場合があります。

`--verbose`で再現し、`TOUCH ALLOW`の許可理由と`TOUCH END`の最大変位を確認してください。`motion`で許可され、`button=0`の場合は、ボタンイベントを検出せず移動条件で転送しています。`--min-move-mm`を大きくすると、この経路での意図しない転送を減らせますが、移動開始時の保留距離も長くなります。

### Initial movement feels less responsive

evdevではOSのタップ認識より前に処理するため、全接触を即時転送してからタップだけを取り消すことはできません。そのため、有効期間外の1本指接触には、既定で1.5mmの移動を判別するまでの保留があります。保留したフレームの再送時刻も、実際の入力時刻とは異なります。

### SYN_DROPPED

入力イベントの欠落を検出すると、状態の不整合を避けるため終了します。保留・フレームバッファが上限に達した場合も終了します。systemdの設定例では、失敗後に自動再起動します。
