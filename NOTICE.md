# 第三方來源與授權文件

此儲存庫包含原廠範例衍生程式、原廠感測器 API，
以及本專案的移植與系統整合程式。
各元件應依其原始著作權標示與適用授權文件使用。

## STM32CubeMP1

基礎套件：STM32CubeMP1 v1.7.0。

M4 範例來源：

```text
Projects/STM32MP157C-DK2/Applications/OpenAMP/OpenAMP_TTY_echo
```

從實際開發套件收集的授權文件位於：

- `licenses/stm32cube/License.md`
- `licenses/stm32cube/Package_license.md`
- `licenses/stm32cube/Package_license.html`
- `licenses/stm32cube/Projects/STM32MP157C-DK2/Applications/`
- `licenses/stm32cube/Drivers/STM32MP1xx_HAL_Driver/`
- `licenses/stm32cube/Drivers/BSP/STM32MP15xx_DISCO/`

## VL53L0X

感測器 API 來自 STSW-IMG005，
版本為 1.0.4、修訂版 4960。

原廠程式位於 `m4-firmware/vl53l0x/vendor/`，
保留原始來源中的授權與著作權標示。

另保存來源檔開頭聲明於：

```text
licenses/VL53L0X-source-notice.txt
```

## Linux 與其他外部依賴

測距平台使用 Linux 核心、SDK、GTK、OpenAMP 及 HAL。
完整套件需另行取得，其授權依相應上游來源及安裝套件為準。

本文件用於記錄來源與授權文件位置，不取代各元件的授權條款。
