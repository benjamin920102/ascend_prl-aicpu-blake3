# AICPU BLAKE3 版本

## 編譯與執行

沿用原本 GitHub Actions 的 CANN 9.0.0 安裝與編譯指令：

```bash
make RANK=128
```

不需要增加 make 參數、CMake 開關或環境變數。既有 Makefile 與
`.github/workflows/main.yml` 完全保留。CMake 會自動使用 AICPU 語言編譯
`kernel/pearl_blake3.aicpu`，並由 ASC linker 與原本的兩個 Ascend C kernel
一起封裝到 `build/libpearl_hlc2.so`。

本次輸出是原始碼 ZIP，未包含預編譯執行檔。需要 CANN 9.0.0 的完整編譯元件，
包括 AICPU CMake 支援、`aicpu_api.h` 與對應 device library。此編輯環境沒有
CANN/BiSheng 或 Ascend NPU。使用者已回報第一版可以完成編譯與啟動，並在
scan-ready event reset 處回傳 207000。本次修正版的完整執行、跨引擎記憶體
可見性與實機 iter/s 仍待設備驗證；主機端測試不能取代這些驗證。

## 運作方式

- Cube 的 Matmul、4-slot GM partial ring 與 producer/consumer flag 協定保留。
- Vector 保留原有 fold 運算與迴圈內 `PIPE_V` barrier 排程，移除
  `GetValue`、scalar BLAKE3 與 target comparison。
- Vector 使用兩塊 4 KiB 的 VECOUT queue buffer 輸出 transcript，避免下一個 band
  覆寫 MTE3 還在讀取的 UB。總 UB 配置約 152 KiB。
- 每個 strip 的完整 transcript 使用獨立 GM 槽位，布局仍為
  `[band][16 words][64 columns]`。診斷 API 的 transcript 布局不變。
- Vector 完成 strip i 後記錄 fold-ready event，AICPU stream 等待該 event，
  再執行一次整批 hash。Vector 可同時繼續 strip i+1；每個 tile 不另行啟動任務。
- AICPU 取得 launch arguments 中的 key/target 快照，對每個有效的 `hi` 執行
  一次 64-byte keyed BLAKE3，使用 unsigned 32-bit 加法與原本的 little-endian
  target comparison。保留 `[strip][hi][band]` 命中編碼。
- AICPU 每次清除全部 live hit record，最多在 record 0 發布一個有效 hit，
  並以 `AscendC::DataStoreBarrier()` 完成結果寫入。
- Host 的槽位完成事件位於 AICPU 與結果 D2H 之後；四個槽位都必須等這個事件
  才能重用。跨 stream 的同步 event 由 `aclrtCreateEventExWithFlag` 建立，
  僅以 `RecordEvent` 再次記錄來重用，不呼叫不支援的 `aclrtResetEvent`。
- `PRL_NOFOLD` 仍僅供診斷，輸出原本的 accumulator prefix；AICPU 跳過 hash，
  清除 hit，不會把無效 transcript 當成有效 share。

預設 `MDIM=16384` 時，新增的 GM transcript 為 1 MiB/strip，四槽共 4 MiB。
只有既有診斷 API 要求時才配置 host transcript 與執行 transcript D2H；
正式路徑只回傳原有的 padded hit record。

## 已執行的檢查

`tests/test_aicpu_blake3.cpp` 可用一般 64-bit C++ compiler 執行，不需 CANN。
檢查官方 64-byte keyed BLAKE3 向量、target 相等與 limb 順序、transcript
strided gathering、命中索引、`n_hi=1/4/64`、不同 key/strip 的結果區重用、
舊 hit 清除、診斷跳過 hash，以及輸入邊界。

```bash
g++ -std=c++11 -O2 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_aicpu_blake3.cpp -o /tmp/test_aicpu_blake3
ASAN_OPTIONS=detect_leaks=0 /tmp/test_aicpu_blake3
```

結果：PASS。`detect_leaks=0` 是因本次測試環境的 `/proc` 限制，AddressSanitizer
與 UndefinedBehaviorSanitizer 仍啟用。沒有測得 NPU 加速幅度，也沒有宣稱此版本
比原版更快。

## 實機確認

1. 用原 GitHub Actions 編譯；啟動時應同時看到 `hash=AICPU` 與
   `events=Ex/no-reset-v2`，可確認實際載入的是本次修正版。
2. 與原版在同一設備、相同 pool/job shape、相同執行條件下比較穩定後 iter/s。
3. 確認 share 仍通過既有 proof/pool 驗證，沒有新增 invalid share 或 timeout。
4. 若使用 profiler，應看到 `pearl_blake3_aicpu` 與下一個 Vector kernel 的重疊。

這是單一 batched AICPU 任務的版本；AICPU 吞吐與排程成本可能成為新瓶頸，
效能改善必須實測。

## 207000 event 修正

第一版誤將 `aclrtCreateEventExWithFlag(..., ACL_EVENT_SYNC)` 建立的事件傳給
`aclrtResetEvent`。這種 Ex event 不支援 reset；支援的重用方式是重新 record，
wait 呼叫則取得當次已記錄的 stream progress。

本次同時移除 scan-ready 與 fold-ready 的兩處 reset，保留以下時序：

- scan-ready：Cube stream record → Vector stream wait；下一次 scan 前須排空槽位。
- fold-ready：Vector stream record → AICPU stream wait → hash → D2H → slot done。
- Host 等到 slot done 才能重用該槽位、重新記錄它的 fold-ready event。

未新增 host 同步等待、編譯開關或效能插樁。Makefile、GitHub Actions、BLAKE3
與 fold 迴圈的運算沒有因本次 event 修正而改變。

## 參考

- [CANN 9.0 AI CPU Operator Compilation](https://www.hiascend.com/doc_center/source/en/CANNCommunityEdition/900/programug/Ascendcopdevg/atlas_ascendc_10_00050.html)
- [DataStoreBarrier](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/ascendcopapi/docs/en/api/AI-CPU-API/DataStoreBarrier.md)
- [官方 BLAKE3 test vectors](https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json)
- [CANN Runtime Event 管理：Ex event 限制與 re-record 語義](https://github.com/hicann/runtime/blob/master/docs/zh/api_ref/07_event_management.md)
