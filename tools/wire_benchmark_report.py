"""Report measured production codecs, envelopes, framing and validation (not the older bare-state benchmark)."""
import argparse
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent.parent
NAMES = ['Move', 'Attack', 'Look', 'Welcome', 'Reply', 'State0', 'State16', 'State256', 'State4096']
FORMATS = ['protobuf', 'flatbuffers', 'memorypack']
OPS = ['EncodeFramed', 'DecodeValidated']
TITLE = {'protobuf': 'Protobuf', 'flatbuffers': 'FlatBuffers', 'memorypack': 'MemoryPack'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--managed', default='out/wire-managed.json')
    parser.add_argument('--native', default='out/wire-native.json')
    parser.add_argument('--out', default='out/wire-report.md')
    args = parser.parse_args()
    managed = json.loads((ROOT / args.managed).read_text(encoding='utf-8'))
    native = json.loads((ROOT / args.native).read_text(encoding='utf-8'))
    cs = {(r['Message'], r['Format'], r['Operation']): r for r in managed['Rows']}
    expected = {(n, f, o) for n in NAMES for f in FORMATS for o in OPS}
    if len(cs) != 54 or set(cs) != expected or len(managed['Rows']) != 54:
        raise ValueError('Managed measurement missing/duplicate cases')
    cpp, allocation = {}, {}
    for run in native['Runs']:
        for field, target in [('TimeRows', cpp), ('AllocationRows', allocation)]:
            rows = {(r['Message'], r['Format'], r['Operation']): r for r in run[field]}
            if len(rows) != 36 or set(rows) != {(n, f, o) for n in NAMES for f in FORMATS[:2] for o in OPS}:
                raise ValueError('Native measurement missing/duplicate cases')
            for key, row in rows.items():
                target.setdefault(key, []).append(row)
    for key in expected:
        row = cs[key]
        if row['MeanNs'] <= 0 or row['AllocatedBytes'] < 0 or not row['SamplesNs']:
            raise ValueError('Invalid managed metrics')
        sibling = cs[key[0], key[1], OPS[1] if key[2] == OPS[0] else OPS[0]]
        if row['WireBytes'] != sibling['WireBytes']:
            raise ValueError('Encode/decode packet size mismatch')
    def c(n, f, o): return cs[n, f, o]
    def med(n, f, o): return statistics.median(float(r['MedianNs']) for r in cpp[n, f, o])
    def alloc(n, f, o):
        rows = allocation[n, f, o]
        values = {(float(r['AllocatedBytes']), float(r['AllocationCount'])) for r in rows}
        if len(values) != 1: raise ValueError('Native allocation probe varied')
        return next(iter(values))
    def speed(before, after):
        return f'{before / after:.2f}배 빠름' if before >= after else f'{after / before:.2f}배 느림'
    lines = ['# 실제 적용 codec의 헤더·검증 포함 직렬화 비교', '',
             f"C# 측정 UTC {managed['Utc']}: {managed['Framework']}, {managed['OS']}, {managed['Architecture']}, logical CPUs={managed['ProcessorCount']}.",
             f"{managed['Mode']}. Release, tiered={managed['TieredCompilation']}, case별 launch {managed['Launches']}회, warmup {managed['WarmupIterations']}회, measurement {managed['MeasurementIterations']}회 × {managed['IterationTimeMs']}ms 목표. 실제 결과 표본과 outlier 처리는 BDN 원문에 보존한다.",
             f"C++ 측정 UTC {native['Utc']}: {native['Platform']}, {native['Architecture']}, {native['Processor']}. {native['Runs'][0]['Logs']['time']}",
             f"Release timing은 allocation hook 없는 실행파일, {native['RunCount']}개 독립 순차 실행 × {native['Samples']} batches × {native['Iterations']} operations, warmup {native['WarmupOperations']}회. 포맷 순서를 교대한다. 표에는 각 실행의 batch median 3개 중앙값과 min–max를 쓴다.", '',
             '## 측정 계약', '',
             '- 기존 benchmark State codec을 재작성하지 않고 실제 SerializerRegistry → WireSerializer → Protobuf/FlatBuffers/MemoryPack adapter를 호출했다. 운영 codec 구현은 변경하지 않았다.',
             '- encode: 공통 DTO → 포맷 생성 타입/bytes → 선택 포맷 8B HXS1 header → 4B big-endian TCP 길이 prefix와 새 소유 frame 복사. 총 wire bytes에 모든 header가 포함된다. Protobuf는 기존 호환 raw 경로로 8B envelope가 없다.',
             '- decode: 이미 완결된 frame의 4B 길이·1MiB 한도 검사 → 포맷/revision/direction/reserved 검사 → 파싱·FlatBuffers verifier 또는 MemoryPack member-length/count 검증 → 전체 소유 DTO/AOI 배열 복원 → 공통 MessageContract/IsValid 의미 검증.',
             '- TCP prefix encode/check는 메모리 내 비용이다. socket I/O, 세션 큐, async Stream 수신/버퍼 누적·패킷 분할, 암호화, MMO tick은 포함하지 않았다. 수신 frame은 미리 만들어 setup에서 재사용하고 encode 결과는 매번 새 소유 buffer다. 전송 계층의 수신 buffer 할당량은 이 표가 아니다.',
             '- Move/Attack/Look client, Welcome/Queued Reply server, State server의 AOI 0/16/256/4096. request_id=123(Welcome=0), entity/target=1234567890123, 좌표=(12.25,-34.5,100.75), HP=87, tick=987654, visible IDs=1000000000000+i. uint64 값 분포가 Protobuf varint 크기에 영향을 준다.',
             '- C#는 MemoryDiagnoser managed allocated B/op. C++는 별도 probe에서 new/new[] 요청 bytes 및 calls/op를 집계한다. aligned/nothrow 포함, direct malloc/OS·외부 runtime 할당은 제외하며 native peak/RSS가 아니다. 두 언어의 alloc 수치를 같은 지표로 합치지 않는다.',
             '- BDN error는 99.9% 신뢰구간의 반폭, 평균 시간은 µs/op다. CPU 스케줄링·GC 등의 변동은 남으며 실제 MMO 전체 지연/처리량으로 일반화하지 않는다.', '',
             '## C# — 실제 codec + frame + validation', '',
             '| 메시지 | 포맷 | 전체 bytes | encode µs ± error | decode µs ± error | encode alloc B/op | decode alloc B/op |',
             '|---|---|---:|---:|---:|---:|---:|']
    for n in NAMES:
        for f in FORMATS:
            e, d = c(n,f,OPS[0]), c(n,f,OPS[1])
            lines.append(f"| {n} | {TITLE[f]} | {e['WireBytes']} | {e['MeanNs']/1000:.3f} ± {e['ErrorNs']/1000:.3f} | {d['MeanNs']/1000:.3f} ± {d['ErrorNs']/1000:.3f} | {e['AllocatedBytes']:.0f} | {d['AllocatedBytes']:.0f} |")
    lines += ['', '## C++ — 실제 codec + frame + validation', '',
              '| 메시지 | 포맷 | 전체 bytes | encode µs (run min–max) | decode µs (run min–max) | encode new B / calls | decode new B / calls |',
              '|---|---|---:|---:|---:|---:|---:|']
    for n in NAMES:
        for f in FORMATS[:2]:
            times = []
            for op in OPS:
                values = [float(r['MedianNs'])/1000 for r in cpp[n,f,op]]
                times.append(f'{statistics.median(values):.3f} ({min(values):.3f}–{max(values):.3f})')
            e, d = alloc(n,f,OPS[0]), alloc(n,f,OPS[1])
            sizes = {int(r['WireBytes']) for r in cpp[n,f,OPS[0]] + allocation[n,f,OPS[0]]}
            if len(sizes) != 1: raise ValueError('Native packet size varied')
            lines.append(f'| {n} | {TITLE[f]} | {next(iter(sizes))} | {times[0]} | {times[1]} | {e[0]:.0f} / {e[1]:.0f} | {d[0]:.0f} / {d[1]:.0f} |')
    lines += ['', '## 선택 결론', '',
              '**클라이언트는 Unity 또는 Unreal, 서버는 독립 C++ 또는 C#이라는 전제다. 공통 wire 기본은 Protobuf, 공통 선택 포맷은 FlatBuffers다. MemoryPack은 공통 필수 프로토콜로 채택하지 않는다.**', '',
              f"1. **작은 명령/응답은 Protobuf 기본이 적절하다.** Look 전체 wire는 Protobuf {c('Look','protobuf',OPS[0])['WireBytes']}B / MemoryPack {c('Look','memorypack',OPS[0])['WireBytes']}B / FlatBuffers {c('Look','flatbuffers',OPS[0])['WireBytes']}B다. 현재 MemoryPack DTO는 모든 body의 공통 고정 필드와 VersionTolerant order를 함께 기록하므로 작은 메시지에도 큰 고정 비용이 남는다. 전송량과 호환성을 함께 보면 모든 명령을 MemoryPack/FlatBuffers로 일괄 전환할 근거가 없다.",
              f"2. **큰 C# 상태(State4096)의 전용 최적화 후보**: MemoryPack은 Protobuf 대비 encode {speed(c('State4096','protobuf',OPS[0])['MeanNs'], c('State4096','memorypack',OPS[0])['MeanNs'])}, decode {speed(c('State4096','protobuf',OPS[1])['MeanNs'], c('State4096','memorypack',OPS[1])['MeanNs'])}. decode alloc은 {c('State4096','memorypack',OPS[1])['AllocatedBytes']:.0f}B 대 {c('State4096','protobuf',OPS[1])['AllocatedBytes']:.0f}B다. 반면 encode alloc은 {c('State4096','memorypack',OPS[0])['AllocatedBytes']:.0f}B 대 {c('State4096','protobuf',OPS[0])['AllocatedBytes']:.0f}B로 더 크다. 이 결과는 .NET 10 전용 경로의 후보 근거다. Helix에는 C++ MemoryPack adapter가 없고 Unity SDK 검증도 없으므로 Unity·Unreal 공통 외부 wire의 선택 근거로 사용하지 않는다.",
              f"3. **공통 FlatBuffers의 큰 상태**: C++ State4096은 Protobuf 대비 encode {speed(med('State4096','protobuf',OPS[0]), med('State4096','flatbuffers',OPS[0]))}, decode {speed(med('State4096','protobuf',OPS[1]), med('State4096','flatbuffers',OPS[1]))}. C# 같은 상태는 encode {speed(c('State4096','protobuf',OPS[0])['MeanNs'], c('State4096','flatbuffers',OPS[0])['MeanNs'])}, decode {speed(c('State4096','protobuf',OPS[1])['MeanNs'], c('State4096','flatbuffers',OPS[1])['MeanNs'])}. 동일 schema여도 C# object API·List/array 복사·검증 비용과 C++ 경로가 다르다. C++에서 유리한 포맷이 현재 C# adapter에서도 같은 비율로 유리하다고 해석하지 않는다.",
              f"4. **대역폭·할당 tradeoff**: State4096 전체 bytes는 Protobuf {c('State4096','protobuf',OPS[0])['WireBytes']} / FlatBuffers {c('State4096','flatbuffers',OPS[0])['WireBytes']} / MemoryPack {c('State4096','memorypack',OPS[0])['WireBytes']}B다. FlatBuffers는 Protobuf보다 {(c('State4096','flatbuffers',OPS[0])['WireBytes']/c('State4096','protobuf',OPS[0])['WireBytes']-1)*100:.1f}% 크며 C# decode alloc은 {c('State4096','flatbuffers',OPS[1])['AllocatedBytes']:.0f}B다. MemoryPack/FlatBuffers fixed uint64 배열은 이 ID 분포에서 varint보다 bytes가 많다.",
              '5. **현재 구현의 개선 후보**: C# FlatBuffers object API의 DTO→List→array 및 UnPack→List→array/입력 bytes 복사를 줄이는 경로, 작은 MemoryPack 메시지의 body별 DTO/union 분리, C++ Protobuf repeated field 사전 용량 확보를 실제 부하와 함께 검토할 수 있다. 이번 작업에서는 최적화나 wire schema 변경을 하지 않고 현재 구현을 측정했다.',
              '6. **이전 결론의 보정**: 이전 bare-state 결과의 MemoryPack/FlatBuffers 배율을 현재 adapter의 성능으로 쓰지 않는다. 이번 표가 header·verifier·VersionTolerant·mapping 포함 기준이다. 세션은 하나의 codec으로 고정되므로 메시지별 혼용을 권장했다고 현재 자동 협상/메시지별 교체가 구현된 것은 아니다.', '',
              '## 빠른 포맷 — 이번 C# 조건에서만', '', '| 메시지 | 가장 빠른 encode | 가장 빠른 validated decode | 가장 작은 wire |', '|---|---|---|---|']
    for n in NAMES:
        e = min(FORMATS, key=lambda f: c(n,f,OPS[0])['MeanNs'])
        d = min(FORMATS, key=lambda f: c(n,f,OPS[1])['MeanNs'])
        b = min(FORMATS, key=lambda f: c(n,f,OPS[0])['WireBytes'])
        lines.append(f'| {n} | {TITLE[e]} | {TITLE[d]} | {TITLE[b]} |')
    lines += ['', '## Unity·Unreal 공통 클라이언트 기준', '',
              '| 용도 | 선택 | 조건 |', '|---|---|---|',
              '| Unity·Unreal 공통 네트워크 메시지 | Protobuf 기본 | 같은 .proto·버전·framing, 언어별 생성 코드 및 client SDK |',
              '| 공통 큰 상태의 선택 최적화 | FlatBuffers | 같은 .fbs·envelope, 실제 Unity IL2CPP/Unreal 빌드에서 검증·크기·전체 복원 비용 확인 |',
              '| C# 전용 데이터 경로 | MemoryPack 선택 | 공통 필수 wire에서 분리, 대상 runtime/SDK와 schema 별도 검증 |', '',
              'C++/C# 표는 각각의 독립 서버·codec 구현 결과다. 서로 다른 언어의 시간을 합친 표를 Unity 또는 Unreal client의 성능으로 사용하지 않는다. 이번 Unity Mono/IL2CPP Player 및 Unreal Shipping 실행은 없었다.',
              '현재 Helix 관리 패키지는 net10.0 서버용이며 Unity SDK로 검증되지 않았다. 공통 .proto/.fbs를 기반으로 클라이언트용 생성 코드·지원 runtime·길이/envelope·요청/응답 codec을 구성하고 엔진별 빌드에서 확인해야 한다. 서버 언어에 따라 외부 메시지 schema를 달리하지 않는다.',
              'MemoryPack 자체에는 Unity 지원이 있지만 현재 Helix VersionTolerant/CustomFormatter adapter와 .NET 10 패키지의 Unity 호환을 뜻하지 않는다. [MemoryPack 1.21.4 Unity 제약](https://github.com/Cysharp/MemoryPack/blob/1.21.4/README.md#unity), [Unity API 호환 수준](https://docs.unity.com/en-us/engine/6000.7/manual/programming-environment/overview-of-dot-net-in-unity/dotnet-profile-support).', '',
              '각 표의 작은 차이는 error/run 변동 범위와 함께 읽는다. C++와 C# 전체 엔진 순위를 만들지 않는다. CPU affinity·운영 부하·다양한 payload 분포·Linux 실행은 이번 범위 밖이다.', '',
              '원시 자료: [C# JSON](wire-managed.json), [C++ JSON](wire-native.json), [BDN 상세 결과](wire-bdn/results/Helix.WireBenchmarks.WireBenchmarks-report-github.md), [wire 계약](../docs/SERIALIZATION.md).', '']
    output = ROOT / args.out
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines), encoding='utf-8')
    print(f'Production wire report: {output}')


if __name__ == '__main__':
    main()
