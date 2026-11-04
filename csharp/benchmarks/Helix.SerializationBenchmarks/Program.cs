using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using BenchmarkDotNet.Attributes;
using BenchmarkDotNet.Running;

namespace Helix.SerializationBenchmarks;

public static class Program
{
	public static State Payload(int count) => new() { EntityId = 1234567890123, X = 12.25, Y = -34.5, Z = 100.75,
		Health = 87, Tick = 987654, VisibleEntityIds = Enumerable.Range(0, count).Select(i => 1000000000000UL + (ulong)i).ToArray() };
	public static void Check(State expected, State actual)
	{
		if (actual.EntityId != expected.EntityId || actual.X != expected.X || actual.Y != expected.Y || actual.Z != expected.Z ||
			actual.Health != expected.Health || actual.Tick != expected.Tick || !actual.VisibleEntityIds.SequenceEqual(expected.VisibleEntityIds))
			throw new InvalidOperationException("Codec changed the domain payload");
	}
	public static void Main(string[] args)
	{
		if (!args.Contains("--quick")) { BenchmarkSwitcher.FromAssembly(typeof(Program).Assembly).Run(args); return; }
		int Integer(string key, int fallback) => Array.IndexOf(args, key) is var at && at >= 0 ? int.Parse(args[at + 1], CultureInfo.InvariantCulture) : fallback;
		var iterations = Integer("--iterations", 5000);
		var samples = Integer("--samples", 9);
		if (iterations < 1 || samples < 3) throw new ArgumentException("iterations > 0 and samples >= 3 required");
		var outputAt = Array.IndexOf(args, "--out");
		var output = outputAt >= 0 ? args[outputAt + 1] : "out/serialization-managed.json";
		var rows = new List<object>();
		Console.WriteLine($"Quick batch measurements: {RuntimeInformation.FrameworkDescription}, {RuntimeInformation.OSDescription}, {RuntimeInformation.ProcessArchitecture}; {samples} samples x {iterations} ops. Use BenchmarkDotNet for decisions.");
		Console.WriteLine("AOI | Codec | Bytes | Encode ns/op | Decode ns/op | Encode allocated B/op | Decode allocated B/op");
		foreach (var count in new[] { 0, 16, 256, 4096 })
		foreach (var kind in Enum.GetValues<CodecKind>())
		{
			var codec = CodecFactory.Create(kind); var state = Payload(count); var bytes = codec.Encode(state);
			Check(state, codec.Decode(bytes));
			for (var i = 0; i < 2000; i++) { GC.KeepAlive(codec.Encode(state)); GC.KeepAlive(codec.Decode(bytes)); }
			var encode = Measure(() => codec.Encode(state), iterations, samples);
			var decode = Measure(() => codec.Decode(bytes), iterations, samples);
			Console.WriteLine(FormattableString.Invariant($"{count} | {codec.Name} | {bytes.Length} | {encode.Nanoseconds:F1} | {decode.Nanoseconds:F1} | {encode.Allocated:F1} | {decode.Allocated:F1}"));
			rows.Add(new { AoiCount = count, Codec = codec.Name, PayloadBytes = bytes.Length, EncodeNs = encode.Nanoseconds, DecodeNs = decode.Nanoseconds,
				EncodeAllocatedBytes = encode.Allocated, DecodeAllocatedBytes = decode.Allocated, EncodeSamplesNs = encode.Samples, DecodeSamplesNs = decode.Samples });
		}
		Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
		File.WriteAllText(output, JsonSerializer.Serialize(new { Mode = "quick batch median, not BenchmarkDotNet", Iterations = iterations, Samples = samples,
			Framework = RuntimeInformation.FrameworkDescription, OS = RuntimeInformation.OSDescription, Architecture = RuntimeInformation.ProcessArchitecture.ToString(),
			TieredCompilation = Environment.GetEnvironmentVariable("DOTNET_TieredCompilation") ?? "runtime default",
			ProcessorCount = Environment.ProcessorCount, Utc = DateTimeOffset.UtcNow, Rows = rows }, new JsonSerializerOptions { WriteIndented = true }));
	}
	private static (double Nanoseconds, double Allocated, double[] Samples) Measure(Func<object> operation, int iterations, int samples)
	{
		var timings = new double[samples]; var allocations = new double[samples];
		for (var batch = 0; batch < samples; batch++)
		{
			GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
			var allocated = GC.GetAllocatedBytesForCurrentThread(); var start = Stopwatch.GetTimestamp();
			for (var i = 0; i < iterations; i++) GC.KeepAlive(operation());
			timings[batch] = Stopwatch.GetElapsedTime(start).TotalNanoseconds / iterations;
			allocations[batch] = (GC.GetAllocatedBytesForCurrentThread() - allocated) / (double)iterations;
		}
		return (timings.Order().ElementAt(samples / 2), allocations.Order().ElementAt(samples / 2), timings);
	}
}

[MemoryDiagnoser]
public class SerializationBenchmarks
{
	[Params(0, 16, 256, 4096)] public int AoiCount { get; set; }
	[ParamsAllValues] public CodecKind Codec { get; set; }
	private IStateCodec _codec = null!;
	private State _state = null!;
	private byte[] _bytes = null!;
	[GlobalSetup] public void Setup()
	{
		_codec = CodecFactory.Create(Codec); _state = Program.Payload(AoiCount); _bytes = _codec.Encode(_state);
		Program.Check(_state, _codec.Decode(_bytes));
	}
	[Benchmark] public byte[] Encode() => _codec.Encode(_state);
	[Benchmark] public State Decode() => _codec.Decode(_bytes);
}
