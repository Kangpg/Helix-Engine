"""Combine measured serialization payload/time/allocation and IOCP/RIO results into Markdown."""
import argparse
import csv
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--managed", default="out/serialization-managed.json")
    parser.add_argument("--native", default="out/serialization-native.csv")
    parser.add_argument("--backend", default="out/backend-benchmark.json")
    parser.add_argument("--baseline", help="Optional previous runtime results with the same payloads and measurement settings")
    parser.add_argument("--out", default="out/serialization-report.md")
    args = parser.parse_args()
    data = json.loads((ROOT / args.managed).read_text(encoding="utf-8"))
    lines = ["# 직렬화 및 Windows I/O 예비 비교", "", f"측정: {data['Utc']}, {data['OS']}, {data['Architecture']}, {data['Framework']}, logical CPUs={data['ProcessorCount']}.",
             f"관리 코드 Release, tiered compilation={data.get('TieredCompilation', 'unknown')}, warmup 2000, {data['Samples']} batch × {data['Iterations']} operations의 중앙값.",
             "C++ Release는 9 batch × 10000 operations 중앙값. 각각 한 프로세스에서 연속 측정했으며 통계적 유의성 검증은 하지 않았다.", "",
             "C++ 및 IOCP/RIO 표는 앞 단계의 저장 결과다. .NET 10 전환 후에는 관리 직렬화를 재측정했으며 각 표의 실행 시점을 구분한다.", "",
             "같은 entity ID, 좌표(double 3개), HP, tick, AOI ID 배열을 사용한다. 직렬화는 새 소유 byte 배열 생성, 역직렬화는 전체 도메인 객체/배열 복원이다. 도메인↔생성 타입 변환을 포함하며 TCP 프레임·네트워크·TLS는 제외한다.",
             "FlatBuffers는 bulk vector/array API를 쓰며 버퍼 뷰만 읽는 비용을 전체 decode와 섞지 않는다. 올바른 payload 비교이며 FlatBuffers의 별도 verifier 비용은 포함하지 않았다. 모든 포맷의 round-trip을 측정 전에 확인한다.", "",
             f"## C# ({data['Framework']})", "", "| AOI IDs | 포맷 | bytes | encode µs | decode µs | encode alloc B/op | decode alloc B/op |", "|---:|---|---:|---:|---:|---:|---:|"]
    for row in data["Rows"]:
        lines.append(f"| {row['AoiCount']} | {row['Codec']} | {row['PayloadBytes']} | {row['EncodeNs']/1000:.3f} | {row['DecodeNs']/1000:.3f} | {row['EncodeAllocatedBytes']:.0f} | {row['DecodeAllocatedBytes']:.0f} |")
    if args.baseline:
        previous = json.loads((ROOT / args.baseline).read_text(encoding="utf-8"))
        for key in ["Iterations", "Samples", "TieredCompilation", "OS", "Architecture"]:
            if previous[key] != data[key]:
                raise ValueError(f"Runtime comparison requires matching {key}")
        before = {(row["AoiCount"], row["Codec"]): row for row in previous["Rows"]}
        after = {(row["AoiCount"], row["Codec"]): row for row in data["Rows"]}
        if before.keys() != after.keys():
            raise ValueError("Runtime comparison requires the same cases")
        lines += ["", f"## 런타임 비교: {previous['Framework']} → {data['Framework']}", "",
                  f"이전 런타임 재측정 UTC={previous['Utc']}, 현재 UTC={data['Utc']}. 한 번씩 순차 실행한 batch median이며 교대 다회 실행/통계적 유의성 검증은 아니다.",
                  "같은 payload·라이브러리 버전·새 배열/전체 객체 복원·Release·tiered=0 조건이다. 음수 변화율은 시간이 감소한 것, 양수는 증가한 것이다. 런타임 업그레이드만으로 모든 포맷이 빨라진다고 해석하지 않는다.", "",
                  "| AOI IDs | 포맷 | encode 이전 µs | encode 현재 µs | 변화 | decode 이전 µs | decode 현재 µs | 변화 |", "|---:|---|---:|---:|---:|---:|---:|---:|"]
        for row in data["Rows"]:
            old = before[row["AoiCount"], row["Codec"]]
            if old["PayloadBytes"] != row["PayloadBytes"] or old["EncodeAllocatedBytes"] != row["EncodeAllocatedBytes"] or old["DecodeAllocatedBytes"] != row["DecodeAllocatedBytes"]:
                raise ValueError("Runtime comparison changed bytes or allocation contract")
            lines.append(f"| {row['AoiCount']} | {row['Codec']} | {old['EncodeNs']/1000:.3f} | {row['EncodeNs']/1000:.3f} | {(row['EncodeNs']/old['EncodeNs']-1)*100:+.1f}% | {old['DecodeNs']/1000:.3f} | {row['DecodeNs']/1000:.3f} | {(row['DecodeNs']/old['DecodeNs']-1)*100:+.1f}% |")
    lines += ["", "## C++ (MSVC x64 Release)", "", "| AOI IDs | 포맷 | bytes | encode µs | decode µs |", "|---:|---|---:|---:|---:|"]
    with (ROOT / args.native).open(encoding="utf-8") as source:
        native_rows = list(csv.DictReader(source))
        for row in native_rows:
            lines.append(f"| {row['AOI']} | {row['Codec']} | {row['Bytes']} | {float(row['EncodeNs'])/1000:.3f} | {float(row['DecodeNs'])/1000:.3f} |")
    backend = json.loads((ROOT / args.backend).read_text(encoding="utf-8"))
    lines += ["", "## IOCP / IOCP+RIO", "", f"같은 native Release TCP echo, {backend['io_threads']} I/O threads, {backend['clients']} clients × {backend['messages']} messages, {backend['samples']} samples. 순서를 번갈아 실행하고 actual backend 이름을 확인했다.",
              "Python asyncio 클라이언트·loopback·프레이밍·스케줄링이 포함되어 순수 커널 I/O 비용이나 대규모 동접 성능으로 해석할 수 없다.", "",
              "| payload bytes | backend | median msg/s | median p95 ms |", "|---:|---|---:|---:|"]
    for size in backend["sizes"]:
        for name in ["iocp", "iocp+rio"]:
            rows = [row for row in backend["rows"] if row["size"] == size and row["backend"] == name]
            lines.append(f"| {size} | {name} | {statistics.median(row['mps'] for row in rows):.0f} | {statistics.median(row['p95'] for row in rows):.2f} |")
    lines += ["", "## 해석 범위", "",
              "- Protobuf는 현재 공통 wire 계약이다. 이 데이터에서는 varint 배열로 패킷 크기가 작지만 변환·파싱·할당 비용이 있다.",
              "- MemoryPack 결과는 C# 런타임/포맷의 결과이며 C++ 공통 wire 포맷을 제공한 것이 아니다.",
              "- FlatBuffers는 generated bulk API·버퍼 소유·검증·view/materialization 선택에 따라 결과가 달라진다.",
              "- Binary는 스키마 진화/필드 태그 없는 고정 LE 비교 기준이다. 운영 프로토콜은 여전히 Protobuf다.",
              "- RIO 구현은 컨텍스트별 등록 버퍼를 재사용하지만 송수신에 staging copy가 있다. 이 표만으로 RIO가 항상 더 빠르다고 결론 내리지 않는다.",
              "- quick batch 결과는 예비 자료다. 최종 직렬화 선택은 BenchmarkDotNet의 별도 프로세스·다회 통계와 실제 게임 패킷 분포로 확인한다.", ""]
    rows = {(row["AoiCount"], row["Codec"]): row for row in data["Rows"]}
    p16, m16 = rows[16, "Protobuf"], rows[16, "MemoryPack"]
    p4k, f4k = rows[4096, "Protobuf"], rows[4096, "FlatBuffers"]
    native = {(int(row["AOI"]), row["Codec"]): row for row in native_rows}
    np, nf = native[4096, "Protobuf"], native[4096, "FlatBuffers"]
    lines += ["## 비교 결론과 Helix 선택", "",
              "현재 데이터에 근거한 엔지니어링 판단이다. 운영 wire/API는 이번 비교로 변경하지 않았다.", "",
              f"1. **C++·C# 공통 기본 wire는 Protobuf 유지**. AOI 16에서 {p16['PayloadBytes']}B, FlatBuffers {rows[16,'FlatBuffers']['PayloadBytes']}B다. AOI 4096에서도 Protobuf {p4k['PayloadBytes']}B vs FlatBuffers {f4k['PayloadBytes']}B로 이 uint64 분포에서는 Protobuf가 {(1-p4k['PayloadBytes']/f4k['PayloadBytes'])*100:.1f}% 작다. 생성 schema·현재 검증·wire 계약을 유지할 가치가 있다. 작은 명령/일반 응답은 이 예비 측정만으로 전체 protocol을 교체하지 않는다.",
              f"2. **C# 전용 고빈도 직렬화 후보는 MemoryPack**. AOI 16의 encode {m16['EncodeNs']/1000:.3f}µs / decode {m16['DecodeNs']/1000:.3f}µs로 Protobuf 대비 각각 {p16['EncodeNs']/m16['EncodeNs']:.1f}배 / {p16['DecodeNs']/m16['DecodeNs']:.1f}배 빠르다. decode 관리 할당도 {m16['DecodeAllocatedBytes']:.0f}B vs {p16['DecodeAllocatedBytes']:.0f}B다. 이 benchmark에는 MemoryPack C++ wire 구현이 없으므로 공통 외부 wire의 직접 대체로 선택하지 않는다. 별도 C# 내부 경로에 적용하기 전 실제 데이터/버전 진화 요구를 확인한다.",
              f"3. **큰 AOI/스냅샷의 공통 binary 후보는 FlatBuffers**. AOI 4096의 전체 decode에서 C#은 Protobuf 대비 {p4k['DecodeNs']/f4k['DecodeNs']:.1f}배, 기존 C++ 측정은 {float(np['DecodeNs'])/float(nf['DecodeNs']):.1f}배 빠르다. 패킷 증가와 verifier/버퍼 소유 비용을 포함하는 실제 전송 측정을 거친 뒤 메시지 종류별 도입을 판단한다. uint64 배열 하나를 복원한 결과를 전체 게임 상태에 그대로 적용하지 않는다.",
              "4. **MessagePack은 범용 binary 대안**, **JSON은 읽기 쉬운 관리/진단용 후보**다. 현재 C# payload에서는 MemoryPack이 MessagePack보다 빠르고, JSON은 가장 큰 payload를 만들었다. 공개 API·디버깅·타언어 도구 호환처럼 다른 요구가 있으면 선택이 달라진다.",
              "5. **직접 Binary는 성능 비교 기준**이다. 이 구현에는 스키마 진화/필드 태그가 없으므로 가장 빠른 행이라는 이유만으로 운영 포맷으로 바꾸지 않는다.",
              "6. **IOCP를 기본, IOCP+RIO를 선택 경로로 유지**한다. 기존 loopback 결과에서 처리량 차이는 작고 Python 클라이언트 제한도 포함된다. 현재 RIO에는 staging copy와 공유 큐 락이 있어 항상 더 빠르다고 주장할 근거가 없다.",
              "", "관련 원리: [Protobuf 개요](https://protobuf.dev/overview/), [MemoryPack 공식 저장소](https://github.com/Cysharp/MemoryPack), [FlatBuffers C# API·검증](https://flatbuffers.dev/languages/c_sharp/).", ""]
    output = ROOT / args.out
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")
    print(f"Report: {output}")


if __name__ == "__main__":
    main()
