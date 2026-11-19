# 重建及部署說明

以下流程以已安裝 STM32MPU Developer Package、A7 SDK、STM32CubeMP1 v1.7.0 及 STM32CubeIDE 的 Ubuntu 主機為前提。

工具預設使用開發時的目錄配置。所有命令皆從本儲存庫根目錄執行。

## 1. A7 圖形程式

```bash
bash tools/build-tof-gui.sh
```

輸出：

```text
build/tof-gui/mp1_tof_monitor
```

若 SDK 安裝在其他位置：

```bash
MP1_SDK="/實際位置/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi" \
    bash tools/build-tof-gui.sh
```

腳本在自己的 shell 內載入 A7 SDK，避免影響 M4 的工具鏈環境。

## 2. 裝置樹

核心來源及已配置的建置目錄應分別位於核心套件下的
`linux-6.6.129/` 與 `build/`。

```bash
bash tools/build-tof-dtb.sh
```

輸出：

```text
build/device-tree/stm32mp157f-dk2-tof-m4.dtb
```

腳本會將本專案 DTS 放到核心的 `arch/arm/boot/dts/st/`，
再建置指定 DTB。若同名 DTS 原先存在且內容不同，會先備份。

可透過 `MP1_DEV`、`MP1_KPKG`、`MP1_SDK` 環境變數指定其他安裝位置。

這個腳本使用既有核心配置。核心需具備 STM32 遠端處理器與
M4 系統資源管理支援，才能載入 M4 韌體並管理測距使用的 I²C5 資源。

## 3. M4 韌體

先檢查來源及官方範例位置：

```bash
python3 tools/restore-cm4.py
```

關閉 CubeIDE 後，再還原發布快照：

```bash
python3 tools/restore-cm4.py --apply
```

其他套件位置可以指定：

```bash
python3 tools/restore-cm4.py \
    --cube "/實際位置/STM32Cube_FW_MP1_V1.7.0" \
    --apply
```

工具會備份目標工程的原檔到 `build/cm4-backups/`，
然後還原主程式、移植層、感測器 API、標頭及工程設定。

接著在 CubeIDE：

1. 開啟或匯入官方範例的 `STM32CubeIDE/CM4` 工程。
2. 選取 `OpenAMP_TTY_echo_CM4`。
3. 重新整理（Refresh）。
4. 清除工程（Clean Project）。
5. 建置工程（Build Project），使用 Debug 配置。

輸出位於官方套件內：

```text
Projects/STM32MP157C-DK2/Applications/OpenAMP/OpenAMP_TTY_echo/
STM32CubeIDE/CM4/Debug/OpenAMP_TTY_echo_CM4.elf
```

本版本以保存的來源與工程設定為準。重新產生 CubeMX 程式碼可能覆蓋手動加入的內容。

## 4. 板端部署位置

| 建置產物或來源 | 板端位置 | 權限 |
|---|---|---|
| 測距 DTB | `/boot/stm32mp157f-dk2-tof-m4.dtb` | 644 |
| A7 圖形程式 | `/usr/local/bin/mp1_tof_monitor` | 755 |
| M4 ELF | `/lib/firmware/OpenAMP_TTY_echo_CM4.elf` 指向的實體檔案 | 可讀取 |
| `systemd/mp1-m4-start.sh` | `/usr/local/sbin/mp1-m4-start.sh` | 755 |
| `systemd/mp1-tof-monitor-wait.sh` | `/usr/local/sbin/mp1-tof-monitor-wait.sh` | 755 |
| `systemd/mp1-m4.service` | `/etc/systemd/system/mp1-m4.service` | 644 |
| `systemd/mp1-tof-monitor.service` | `/etc/systemd/system/mp1-tof-monitor.service` | 644 |

目前板子的韌體連結指向：

```text
/usr/local/projects/OpenAMP_TTY_echo_CM4/lib/firmware/OpenAMP_TTY_echo_CM4.elf
```

更新時先確認實際連結：

```bash
readlink -f /lib/firmware/OpenAMP_TTY_echo_CM4.elf
```

先停止圖形程式及 M4，再備份並替換韌體實體檔案。
使用暫存檔完成傳送，確認傳送成功後再移到正式位置。

## 5. 開機裝置樹選擇

已驗證板子的開機分割區是 `/boot`，設定檔是：

```text
/boot/mmc0_extlinux/stm32mp157f-dk2_extlinux.conf
```

在保留原始 OpenSTLinux 項目及設定備份的前提下，
加入名為 `mp1-tof-m4` 的項目，使用：

| 設定 | 值 |
|---|---|
| LABEL | `mp1-tof-m4` |
| KERNEL | `/uImage` |
| FDT | `/stm32mp157f-dk2-tof-m4.dtb` |
| INITRD | `/st-image-resize-initrd` |
| APPEND | 沿用該板原始 OpenSTLinux 項目的完整內容 |
| DEFAULT | `mp1-tof-m4` |

根檔案系統的 PARTUUID 必須沿用實際 SD 卡設定，不能直接套用其他板子的值。

重新開機後，確認 Linux I²C5 節點為 disabled，
M4 的 I²C5 資源節點為 okay，PA11／PA12 已由 M4 資源節點保留，
再啟動測距韌體。

## 6. 啟動服務

啟動測距服務前，確認同一組 RPMsg 串口沒有被其他程式占用，
避免量測資料或控制回覆被不同程序讀取。

完成檔案部署與權限設定後，在板子執行：

```bash
systemctl daemon-reload
systemctl enable --now mp1-m4.service
systemctl enable --now mp1-tof-monitor.service
```

圖形服務使用：

- 使用者與群組：`weston`
- 附加群組：`dialout`
- `XDG_RUNTIME_DIR=/run/user/1000`
- `WAYLAND_DISPLAY=wayland-0`
- `GDK_BACKEND=wayland`

若映像的 Weston 使用者或執行目錄不同，需同步調整服務與等待腳本。

## 7. 檢查

在板子執行：

```bash
cat /sys/class/remoteproc/remoteproc0/firmware
cat /sys/class/remoteproc/remoteproc0/state

systemctl --no-pager --full status \
    mp1-m4.service \
    mp1-tof-monitor.service

journalctl -b -u mp1-tof-monitor.service --no-pager -n 30
```

預期 M4 狀態為 `running`，圖形服務為 `active (running)`。
視窗初始保持停止；按 START 後顯示量測，按 STOP 後停止新增量測。
