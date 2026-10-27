# AICPU 僅前處理，Vector 完成 BLAKE3

沿用原 GitHub Actions 的 CANN 9.0.0 與 `make RANK=128`，不需新增編譯參數或開關。
啟動標記：`hash=Vector prep=AICPU/HBM prep_bytes=128 prep_scope=scan events=Ex/no-reset-v3`。

## 分工

| 執行端 | 工作 | 頻率 |
| --- | --- | --- |
| AICPU | 初始 compression state、第一輪 4 個 key-only 加法、target | 每次更新 key/target 一次 |
| HBM/GM | 保存 128-byte prepared context，直到全部 strip 完成 | 同一個 scan 共用 |
| Vector scalar | 讀取 context，對 transcript 完成七輪 mixing 與 target comparison | 每 kernel 讀一次 context；每 tile hash 一次 |
| Vector 向量單元 | 原本的 Add、fold、rotate、XOR | 與原版相同 |

HBM context 的 word offset：

| Offset | 內容 |
| --- | --- |
| 0–15 | 8 個 key words、4 個 IV words、counter=0、length=64、flags=27 |
| 16–19 | `key[i] + key[i+4]`，unsigned 32-bit modulo 加法 |
| 20–27 | little-endian target limbs |
| 28–31 | 0 padding |

AICPU 不讀 transcript，不執行 message mixing，也不掃描 hit。Vector 使用預算的
4 個 key sums 開始第一輪 column mixing，再完成第一輪 diagonal mixing 與其餘六輪。
每個 hash 共省下 4 個 key-only 加法。這是很小的前處理，不能視為移走大部分 hash 工作，
也不能保證比原本的純 Vector 版更快。

## 同步與生命週期

1. 先前 scan 的 strip queue 排空後，才允許更新 key/target。
2. `pearl_set_key_target` 將 key/target 快照傳入 `pearl_blake3_precompute`，每 scan
   一次；AICPU 在獨立 prep stream 寫入 HBM，呼叫 `DataStoreBarrier()` 後記錄 prep-ready。
3. Vector stream 分別等待 flag reset 與 prep-ready。Cube 的 Matmul 可以同時開始。
4. 每個 Vector kernel 開始時刷新 context 的 scalar cache line，將 initial state、key
   sums 與 target 讀入私有 scalar 陣列，之後所有 tile 共用，不再逐 tile/strip 等待 AICPU。
5. Vector 完成 fold/hash、結果 D2H 後記錄原有完成事件。全部 strip 完成前不覆寫 context。

兩個同步事件都以 `aclrtCreateEventExWithFlag(..., ACL_EVENT_SYNC)` 建立，透過重新
`RecordEvent` 重用，不呼叫 `aclrtResetEvent`，保留 207000 修正。

正式路徑的 transcript 留在 UB；新增 HBM 資料只有 128 bytes 的 prepared context。
完整 transcript GM 配置與 D2H 僅在原本診斷 API 要求時使用。原本的 partial ring、
fold loop、`PIPE_V` 排程、look-ahead prefetch 與 per-worker hit 編碼保留。

## 驗證

- 官方 64-byte keyed BLAKE3 test vector 通過。
- 與原始上傳版本的完整 Vector BLAKE3 差分比較：4,096 組隨機 key/input/target
  加 2 組邊界，每組 4 個 transcript，共 16,392 個 hash，全部相同。
- 128-byte ABI、counter/length/flags、32-bit 加法溢位、target 更新與 padding 檢查通過。
- 主機端 AddressSanitizer、UndefinedBehaviorSanitizer 測試通過。
- Makefile 與 GitHub Actions 與原上傳檔逐 byte 相同；fold loop 運算與 barrier 排程相同。

可選的主機端測試指令（不影響正式 make 流程）：

```bash
g++ -std=c++11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_blake3_precompute.cpp -o /tmp/test_blake3_precompute
ASAN_OPTIONS=detect_leaks=0 /tmp/test_blake3_precompute
```

LeakSanitizer 因本次環境的 `/proc` 限制關閉；ASan 與 UBSan 仍啟用。
此編輯環境沒有 CANN/BiSheng 與 NPU，本次版本的完整編譯、跨引擎記憶體可見性、
share 驗證與實機 iter/s 仍需設備驗證。先前完整 AICPU hash 版本已由使用者回報較慢。

## 監測數據

使用者回報完整 AICPU hash 版約 AI Cpu 16%、AI Cube 9–16%、NPU Util 100%。
這些整體百分比不能單獨證明瓶頸；單個 AICPU 任務可能受少數核心或排程限制。
此版本不再逐 strip 提交 AICPU hash，應以穩定後 iter/s 與 profiler 確認效果。

## 參考

- [官方 BLAKE3 reference](https://github.com/BLAKE3-team/BLAKE3/blob/master/reference_impl/reference_impl.rs)
- [官方 BLAKE3 test vectors](https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json)
- [CANN AI CPU 編譯](https://www.hiascend.com/doc_center/source/en/CANNCommunityEdition/900/programug/Ascendcopdevg/atlas_ascendc_10_00050.html)
- [CANN Event 管理與 Ex event 限制](https://github.com/hicann/runtime/blob/master/docs/zh/api_ref/07_event_management.md)
