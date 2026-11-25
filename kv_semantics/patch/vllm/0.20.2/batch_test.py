#!/usr/bin/env python3
"""
TPOT 自动调优脚本 - 20% 递增策略
从初始并发数开始每次增加 20%，直到吞吐连续下降 2 次则认为系统已打满，停止并跳转到下一个用例。

用法:
    python3 tpot_tuner.py
    python3 tpot_tuner.py --cases "32,1,100;16,2,50"
    python3 tpot_tuner.py --max-cc 4096
    python3 tpot_tuner.py --dataset-type normal --repeat-rate 0 --no-prefix-test --dp 1
    python3 tpot_tuner.py --env "TORCH_DEVICE_BACKEND_AUTOLOAD=0"
"""

import os
import re
import sys
import logging
import argparse
import subprocess
from typing import Optional, List, Tuple

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)

# ============================================================
# 可编辑的测试用例表 (K 单位, 运行时会 *1024)
# ============================================================
K_UNIT = 1024

DEFAULT_TEST_CASES: List[Tuple[int, int, int]] = [
    #(1,   1,   1),
    (4,   1,   10),
    #(8,   1,   1),
    #(16,  1,   1),
    #(32,  1,   1),
    #(32,  4,   1),
    #(50,  1,   1),
    #(64,  1,   1),
    #(128, 1,   1),
]

# ============================================================
# 可配置参数
# ============================================================
RUN_CONFIG = {
    "dataset_type": "prefix_cache",
    "repeat_rate": "0.5,0.7,0.9",
    "prefix_test": True,
    "dp": 1,
    "env_prefix": os.environ.get("AISBENCH_ENV_PREFIX", ""),
}

MIN_CONCURRENCY = 1
MAX_CONCURRENCY = 1000
DATA_NUM_MULTIPLIER = 3
INCREMENT_RATIO = 1.1  # 每次递增比例

AISBENCH_LOG = "aisbench.log"


# ============================================================
# 日志解析 — 全量指标
# ============================================================

def parse_all_metrics(log_path: str = AISBENCH_LOG) -> dict:
    """从 aisbench.log 解析全部指标，与 save_file.py 逻辑一致"""
    result = {
        "ttft_avg": None, "ttft_p90": None,
        "tpot_avg": None, "tpot_p90": None,
        "benchmark_duration_s": None,
        "concurrency": None, "max_concurrency": None,
        "output_token_throughput": None,
        "input_token_throughput": None,
        "total_token_throughput": None,
        "input_tokens": None, "output_tokens": None,
        "total_requests": None,
        "request_throughput_qps": None,
        "prefill_token_throughput": None,
    }
    if not os.path.exists(log_path):
        return result
    try:
        with open(log_path, "r", encoding="utf-8") as f:
            for line in f:
                if "TTFT" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["ttft_avg"] = float(m[0])
                        result["ttft_p90"] = float(m[5]) if len(m) >= 6 else None
                elif "TPOT" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["tpot_avg"] = float(m[0])
                        result["tpot_p90"] = float(m[5]) if len(m) >= 6 else None
                elif "Benchmark Duration" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["benchmark_duration_s"] = float(m[0]) / 1000
                elif "Max Concurrency" in line:
                    m = re.findall(r"[\w']+", line)
                    if m:
                        try:
                            result["max_concurrency"] = float(m[-1])
                        except ValueError:
                            pass
                elif "Concurrency" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["concurrency"] = float(m[0])
                elif "Output Token Throughput" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["output_token_throughput"] = float(m[0])
                elif "Input Token Throughput" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["input_token_throughput"] = float(m[0])
                elif "Total Token Throughput" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["total_token_throughput"] = float(m[0])
                elif "InputTokens" in line:
                    m = re.findall(r"(\d+\.?\d*)", line)
                    if m:
                        result["input_tokens"] = float(m[0])
                elif "OutputTokens" in line:
                    m = re.findall(r"(\d+\.?\d*)", line)
                    if m:
                        result["output_tokens"] = float(m[0])
                elif "Total Requests" in line:
                    m = re.findall(r"(\d+\.?\d*)", line)
                    if m:
                        result["total_requests"] = float(m[0])
                elif "Request Throughput" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["request_throughput_qps"] = float(m[0])
                elif "Prefill Token Throughput" in line:
                    m = re.findall(r"(\d+\.\d+)", line)
                    if m:
                        result["prefill_token_throughput"] = float(m[0])
    except Exception:
        pass
    return result


# 便捷访问函数 (兼容旧接口)
def parse_tpot_from_log(log_path: str = AISBENCH_LOG) -> Optional[float]:
    return parse_all_metrics(log_path)["tpot_avg"]

def parse_total_token_throughput(log_path: str = AISBENCH_LOG) -> Optional[float]:
    return parse_all_metrics(log_path)["total_token_throughput"]

def parse_reported_concurrency(log_path: str = AISBENCH_LOG) -> Optional[float]:
    return parse_all_metrics(log_path)["concurrency"]

def parse_max_concurrency(log_path: str = AISBENCH_LOG) -> Optional[float]:
    return parse_all_metrics(log_path)["max_concurrency"]


TPOT_UPPER_LIMIT = 50.0  # tpot 超过此值认为打满（全输入统一）


def get_ttft_threshold(input_len: int) -> float:
    """根据 input_len 返回该档位 TTFT 上限 (ms)"""
    if input_len <= 4096:        # 1K, 2K, 4K → 2s
        return 2000.0
    elif input_len <= 8192:      # 8K → 10s
        return 10000.0
    elif input_len <= 16384:     # 16K → 20s
        return 20000.0
    elif input_len <= 32768:     # 32K → 30s
        return 30000.0
    elif input_len <= 51200:     # 50K → 40s
        return 40000.0
    elif input_len <= 65536:     # 64K → 45s
        return 45000.0
    elif input_len <= 131072:     # 64K → 45s
            return 60000.0
    else:                        # 128K+ → 100s
        return 100000.0


# ============================================================
# 命令构建
# ============================================================

def build_command(input_len: int, output_len: int, data_num: int,
                  concurrency: int) -> str:
    parts = []
    env = RUN_CONFIG["env_prefix"]
    if env:
        parts.append(env)
    parts.append("python aisbench_test.py")
    parts.append(f"--input_len {input_len}")
    parts.append(f"--output_len {output_len}")
    parts.append(f"--data_num {data_num}")
    parts.append(f"--concurrency {concurrency}")
    parts.append(f"--dataset_type {RUN_CONFIG['dataset_type']}")
    parts.append(f"--repeat_rate {RUN_CONFIG['repeat_rate']}")
    if RUN_CONFIG["prefix_test"]:
        parts.append("--prefix_test")
    parts.append(f"--dp {RUN_CONFIG['dp']}")
    return " ".join(parts)


# ============================================================
# 说明：batch_test 不再生成结果 CSV
# 结果文件由 aisbench_test.py 写入：aisbench_result_r<命中率>.csv
# ============================================================


# ============================================================
# BenchmarkRunner
# ============================================================

class BenchmarkRunner:
    def run_benchmark(self, input_len: int, output_len: int,
                      concurrency: int, data_num: int,
                      timeout: int = 7200) -> Optional[Tuple[float, Optional[float]]]:
        cmd = build_command(input_len, output_len, data_num, concurrency)
        logging.info(f"[RUN] {cmd}")
        try:
            result = subprocess.run(cmd, shell=True, capture_output=False,
                                    text=True, timeout=timeout)
            if result.returncode != 0:
                logging.warning(f"命令返回非零退出码: {result.returncode}")
        except subprocess.TimeoutExpired:
            logging.error(f"命令超时 ({timeout}s)")
            return None
        except Exception as e:
            logging.error(f"命令执行异常: {e}")
            return None

        tpot = parse_tpot_from_log()
        throughput = parse_total_token_throughput()
        if tpot is not None:
            return (tpot, throughput)
        return None


# ============================================================
# 20% 递增搜索器
# ============================================================

class IncrementalSearcher:
    """每次增加指定比例并发数，打满即停"""

    def __init__(self, runner: BenchmarkRunner,
                 min_cc: int = MIN_CONCURRENCY,
                 max_cc: int = MAX_CONCURRENCY,
                 data_multiplier: int = DATA_NUM_MULTIPLIER,
                 max_retries: int = 1,
                 increment_ratio: float = INCREMENT_RATIO):
        self.runner = runner
        self.min_cc = min_cc
        self.max_cc = max_cc
        self.data_multiplier = data_multiplier
        self.max_retries = max_retries
        self.increment_ratio = increment_ratio

    def _test_cc(self, input_len: int, output_len: int,
                 concurrency: int) -> Optional[Tuple[float, Optional[float]]]:
        data_num = concurrency * self.data_multiplier
        for attempt in range(self.max_retries + 1):
            result = self.runner.run_benchmark(
                input_len, output_len, concurrency, data_num)
            if result is not None:
                tpot, throughput = result
                return result
            if attempt < self.max_retries:
                logging.warning(f"重试 ({attempt + 1}/{self.max_retries})...")
        return None

    def search(self, input_len: int, output_len: int,
               initial_cc: int = 0) -> dict:
        logging.info(f"{'='*60}")
        logging.info(f"[CASE] input_len={input_len} output_len={output_len} "
                     f"init_cc={initial_cc}")
        logging.info(f"{'='*60}")

        iterations = 0
        best_cc = initial_cc if initial_cc > 0 else self.min_cc
        best_tpot: Optional[float] = None
        ttft_limit = get_ttft_threshold(input_len)

        # Phase 1: 测试初始值
        if 0 < initial_cc <= self.max_cc:
            iterations += 1
            result = self._test_cc(input_len, output_len, initial_cc)
            if result is not None:
                tpot, throughput = result
                best_tpot = tpot
                metrics = parse_all_metrics()
                ttft = metrics.get("ttft_avg")
                tp = (f"throughput={throughput:.2f}"
                      if throughput is not None else "")
                logging.info(f"  iter={iterations} cc={initial_cc} "
                             f"tpot={tpot:.2f}ms {tp}")

                # 初始值就打满 (tpot > 50ms 或 ttft 超过档位上限)
                if (tpot > TPOT_UPPER_LIMIT or
                    (ttft is not None and ttft > ttft_limit)):
                    trigger = f"TTFT>{ttft_limit:.0f}ms" if (ttft is not None and ttft > ttft_limit) else "TPOT>50ms"
                    logging.info(f"[SATURATED] 初始值 tpot={tpot:.2f}ms "
                                 f"ttft={ttft} > {trigger}，系统已打满")
                    return self._result(input_len, output_len, initial_cc,
                                        initial_cc, tpot, iterations,
                                        "throughput_saturated")
            else:
                logging.warning("初始值测试失败")
        else:
            initial_cc = self.min_cc

        # Phase 2: 每次增加 20%，直到打满或触及上限
        cc = initial_cc
        last_good_cc = best_cc
        while cc <= self.max_cc:
            nxt = int(cc * self.increment_ratio) + 1
            if nxt <= cc:
                nxt = cc + 1
            cc = nxt
            if cc > self.max_cc:
                break

            iterations += 1
            result = self._test_cc(input_len, output_len, cc)
            if result is None:
                logging.error(f"cc={cc} 测试失败")
                break

            tpot, throughput = result
            metrics = parse_all_metrics()
            ttft = metrics.get("ttft_avg")

            # 打满检测: tpot > 50ms 或 ttft 超过档位上限
            if (tpot > TPOT_UPPER_LIMIT or
                    (ttft is not None and ttft > ttft_limit)):
                trigger = f"TTFT>{ttft_limit:.0f}ms" if (ttft is not None and ttft > ttft_limit) else "TPOT>50ms"
                logging.info(f"[SATURATED] tpot={tpot:.2f}ms ttft={ttft} > "
                             f"{trigger}，系统已打满 "
                             f"(最优 cc={last_good_cc})")
                return self._result(input_len, output_len, initial_cc,
                                    last_good_cc, best_tpot or tpot, iterations,
                                    "throughput_saturated")

            last_good_cc = cc
            best_tpot = tpot
            tp = (f"throughput={throughput:.2f}"
                  if throughput is not None else "")
            logging.info(f"  iter={iterations} cc={cc} "
                         f"tpot={tpot:.2f}ms {tp}")

        logging.info(f"[MAX] 达到最大并发数 {self.max_cc}，未打满")
        return self._result(input_len, output_len, initial_cc,
                            last_good_cc, best_tpot or 0, iterations,
                            "max_reached")

    def _result(self, il: int, ol: int, init_cc: int, opt_cc: int,
                tpot: float, iters: int, status: str,
                metrics: Optional[dict] = None) -> dict:
        m = metrics or {}
        return {
            "input_len": il,
            "output_len": ol,
            "initial_concurrency": init_cc,
            "optimal_concurrency": opt_cc,
            "final_tpot_ms": round(tpot, 2),
            "iterations": iters,
            "status": status,
            "ttft_avg_ms": m.get("ttft_avg"),
            "ttft_p90_ms": m.get("ttft_p90"),
            "tpot_p90_ms": m.get("tpot_p90"),
            "benchmark_duration_s": m.get("benchmark_duration_s"),
            "max_concurrency": m.get("max_concurrency"),
            "concurrency_reported": m.get("concurrency"),
            "output_token_throughput": m.get("output_token_throughput"),
            "total_token_throughput": m.get("total_token_throughput"),
            "input_tokens": m.get("input_tokens"),
            "output_tokens": m.get("output_tokens"),
            "total_requests": m.get("total_requests"),
            "request_throughput_qps": m.get("request_throughput_qps"),
        }


# 兼容旧名称
BinarySearchTuner = IncrementalSearcher


# ============================================================
# 编排器
# ============================================================

class TpotTuner:
    def __init__(self, test_cases: List[Tuple[int, int, int]],
                 searcher: IncrementalSearcher):
        self.test_cases = test_cases
        self.searcher = searcher
        self.results: List[dict] = []

    def run(self) -> List[dict]:
        total = len(self.test_cases)
        for idx, (il, ol, init_cc) in enumerate(self.test_cases, 1):
            logging.info(f"\n{'#'*60}")
            logging.info(f"# 进度: {idx}/{total}")
            logging.info(f"{'#'*60}")
            result = self.searcher.search(il, ol, init_cc)
            self.results.append(result)
            self._print_result(result)
        self._print_summary()
        return self.results

    def _print_result(self, r: dict):
        labels = {
            "throughput_saturated": "[SAT]",
            "max_reached": "[MAX]",
            "error": "[ERR]",
        }
        label = labels.get(r["status"], "[?]")
        logging.info(f"  {label} il={r['input_len']} ol={r['output_len']} "
                     f"opt_cc={r['optimal_concurrency']} "
                     f"tpot={r['final_tpot_ms']}ms iters={r['iterations']}")

    def _print_summary(self):
        logging.info(f"\n{'='*60}")
        logging.info("SUMMARY")
        logging.info(f"{'='*60}")
        hdr = (f"{'input_len':>10} {'output_len':>10} {'init_cc':>8} "
               f"{'opt_cc':>8} {'tpot_ms':>10} {'iters':>6} {'status':>22}")
        logging.info(hdr)
        logging.info("-" * len(hdr))
        for r in self.results:
            logging.info(
                f"{r['input_len']:>10} {r['output_len']:>10} "
                f"{r['initial_concurrency']:>8} {r['optimal_concurrency']:>8} "
                f"{r['final_tpot_ms']:>10.2f} {r['iterations']:>6} "
                f"{r['status']:>22}"
            )
        logging.info(f"{'='*60}")


# ============================================================
# CLI
# ============================================================

def parse_repeat_rates(rates_str: str) -> List[float]:
    """逗号分隔多值: "0,0.2,0.5,1.0" 或 "0%,20%,50%,100%" -> [0.0, 0.2, 0.5, 1.0]"""
    rates = []
    for r in str(rates_str).split(","):
        r = r.strip()
        if r.endswith("%"):
            v = float(r[:-1]) / 100.0
        else:
            v = float(r)
        if not (0.0 <= v <= 1.0):
            raise ValueError(f"命中率 {r} 不在 [0,1] 区间")
        rates.append(v)
    return rates


def parse_cases(cases_str: str) -> List[Tuple[int, int, int]]:
    cases = []
    for part in cases_str.split(";"):
        part = part.strip()
        if not part:
            continue
        vals = part.split(",")
        if len(vals) != 3:
            raise ValueError(f"无效格式: '{part}'，应为 'ilK,olK,cc'")
        cases.append((int(vals[0]), int(vals[1]), int(vals[2])))
    return cases


def main():
    parser = argparse.ArgumentParser(
        description="TPOT 自动调优 - 20% 递增，吞吐打满即停",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="示例:\n"
               "  python3 tpot_tuner.py\n"
               "  python3 tpot_tuner.py --cases \"32,1,100;16,2,50\"\n"
               "  python3 tpot_tuner.py --max-cc 2048\n"
               "  python3 tpot_tuner.py --repeat-rate 0,0.2,0.5,0.7,1.0\n"
               "  python3 tpot_tuner.py --env \"TORCH_DEVICE_BACKEND_AUTOLOAD=0\"\n"
               "  python3 tpot_tuner.py --dataset-type normal --no-prefix-test --dp 1",
    )
    parser.add_argument("--min-cc", type=int, default=MIN_CONCURRENCY)
    parser.add_argument("--max-cc", type=int, default=MAX_CONCURRENCY)
    parser.add_argument("--cases", type=str, default=None,
                        help='测试用例 (K单位): "ilK,olK,cc;..."')
    parser.add_argument("--env", type=str, default=None,
                        help="环境变量前缀")
    parser.add_argument("--max-retries", type=int, default=1)
    parser.add_argument("--increment-ratio", type=float, default=INCREMENT_RATIO,
                        help=f"每次并发递增比例 (默认 {INCREMENT_RATIO})")
    parser.add_argument("--dataset-type", type=str,
                        default=RUN_CONFIG["dataset_type"])
    parser.add_argument("--repeat-rate", type=str,
                        default=RUN_CONFIG["repeat_rate"])
    parser.add_argument("--prefix-test", action="store_true",
                        default=RUN_CONFIG["prefix_test"])
    parser.add_argument("--no-prefix-test", action="store_true", default=False)
    parser.add_argument("--dp", type=int, default=RUN_CONFIG["dp"])
    args = parser.parse_args()

    prefix_test = args.prefix_test
    if args.no_prefix_test:
        prefix_test = False

    if args.env is not None:
        RUN_CONFIG["env_prefix"] = args.env
    RUN_CONFIG["dataset_type"] = args.dataset_type
    RUN_CONFIG["prefix_test"] = prefix_test
    RUN_CONFIG["dp"] = args.dp

    repeat_rates = parse_repeat_rates(args.repeat_rate)
    total_rates = len(repeat_rates)

    if args.cases:
        try:
            test_cases = parse_cases(args.cases)
        except ValueError as e:
            logging.error(str(e))
            sys.exit(1)
    else:
        test_cases = DEFAULT_TEST_CASES

    # K -> actual
    test_cases = [(il * K_UNIT, ol * K_UNIT, cc) for il, ol, cc in test_cases]

    if not test_cases:
        logging.error("没有测试用例")
        sys.exit(1)

    logging.info(f"并发数范围: [{args.min_cc}, {args.max_cc}]")
    logging.info(f"data_num = concurrency * {DATA_NUM_MULTIPLIER}")
    logging.info(f"数据集参数: --dataset_type {args.dataset_type} "
                 f"--repeat_rates {repeat_rates} "
                 f"--prefix_test={prefix_test} --dp {args.dp}")
    if RUN_CONFIG["env_prefix"]:
        logging.info(f"环境前缀: {RUN_CONFIG['env_prefix']}")
    logging.info(f"测试用例数量: {len(test_cases)}")
    for il, ol, cc in test_cases:
        logging.info(f"  il={il} ({il // K_UNIT}K), ol={ol} ({ol // K_UNIT}K), init_cc={cc}")

    for rate_idx, repeat_rate in enumerate(repeat_rates):
        RUN_CONFIG["repeat_rate"] = str(repeat_rate)

        logging.info(f"{'='*60}")
        logging.info(f"[{rate_idx+1}/{total_rates}] 开始测试 命中率={repeat_rate}")
        logging.info(f"  结果请查看 aisbench_test.py 生成的 aisbench_result_r{repeat_rate}.csv")
        logging.info(f"{'='*60}")

        runner = BenchmarkRunner()
        searcher = IncrementalSearcher(
            runner=runner,
            min_cc=args.min_cc,
            max_cc=args.max_cc,
            data_multiplier=DATA_NUM_MULTIPLIER,
            max_retries=args.max_retries,
            increment_ratio=args.increment_ratio,
        )
        tuner = TpotTuner(test_cases, searcher)

        try:
            tuner.run()
        except KeyboardInterrupt:
            logging.info("\n中断信号，结束运行")
            sys.exit(1)

        # 清理 picked_ids，确保下一个命中率从头挑选数据
        for fname in ("picked_ids.txt",):
            if os.path.exists(fname):
                os.remove(fname)
                logging.info(f"  已清理 {fname}")

        logging.info(f"[{rate_idx+1}/{total_rates}] 命中率={repeat_rate} 完成")


if __name__ == "__main__":
    main()
